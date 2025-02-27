﻿/**
 * Tencent is pleased to support the open source community by making Tars available.
 *
 * Copyright (C) 2016THL A29 Limited, a Tencent company. All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use this file except 
 * in compliance with the License. You may obtain a copy of the License at
 *
 * https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software distributed 
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR 
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the 
 * specific language governing permissions and limitations under the License.
 */

#include <iterator>
#include <algorithm>
#include "DbHandle.h"
#include "RegistryServer.h"
#include "LoadBalanceThread.h"
#include "util.h"
#include "NodeManager.h"

TC_ReadersWriterData<ObjectsCache> CDbHandle::_objectsCache;

TC_ReadersWriterData<std::map<int, CDbHandle::GroupPriorityEntry> > CDbHandle::_mapGroupPriority;

std::map<ServantStatusKey, int> CDbHandle::_mapServantStatus;
std::map<ServantStatusKey, int> CDbHandle::_mapServantFlowStatus;

TC_ThreadLock CDbHandle::_mapServantStatusLock;
TC_ThreadLock CDbHandle::_mapServantFlowStatusLock;

//map<string, NodePrx> CDbHandle::_mapNodePrxCache;
//TC_ThreadLock CDbHandle::_NodePrxLock;

//key-ip, value-组编号
TC_ReadersWriterData<map<string, int> > CDbHandle::_groupIdMap;
//key-group_name, value-组编号
TC_ReadersWriterData<map<string, int> > CDbHandle::_groupNameMap;

TC_ReadersWriterData<CDbHandle::SetDivisionCache> CDbHandle::_setDivisionCache;

tars::TC_Mysql CDbHandle::_mysqlQueryStat;
bool CDbHandle::_isMysqlQueryStatInited = false;

extern RegistryServer g_app;
extern TC_Config *g_pconf;

int CDbHandle::init(TC_Config *pconf)
{
    try
    {
        TC_DBConf tcDBConf;
        tcDBConf.loadFromMap(pconf->getDomainMap("/tars/db"));
        string option = pconf->get("/tars/db<dbflag>", "");
        if (!option.empty() && option == "CLIENT_MULTI_STATEMENTS")
        {
            tcDBConf._flag = CLIENT_MULTI_STATEMENTS;
            _enMultiSql = true;
            TLOG_DEBUG("CDbHandle::init tcDBConf._flag: " << option << endl);
        }
        _mysqlReg.init(tcDBConf);

        if (!_isMysqlQueryStatInited)
        {
            _isMysqlQueryStatInited = true;
            TC_DBConf statDBConf;
            statDBConf.loadFromMap(pconf->getDomainMap("/tars/querystatdb"));
            _mysqlQueryStat.init(statDBConf);
        }
    }
    catch (TC_Config_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::init exception: " << ex.what() << endl);
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::init exception: " << ex.what() << endl);
        exit(0);
    }

    return 0;
}

void CDbHandle::updateMysql()
{
    try
    {
        _mysqlReg.execute("alter table `t_server_conf` add `flow_state` enum ('active','inactive') NOT NULL DEFAULT 'active'");
    }
    catch(const std::exception& e)
    {
    }
}

int CDbHandle::loadDockerInfo(map<string, DockerRegistry> &info)
{
	try
	{
		{
			string sSelectSql = "select * from t_docker_registry";

			TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSelectSql);

			for (size_t i = 0; i < res.size(); i++)
			{
				DockerRegistry registry;

				registry.sId = res[i]["id"];
				registry.sRegistry = res[i]["registry"];
				registry.sUserName = res[i]["username"];
				registry.sPassword = res[i]["password"];
				registry.bSucc = false;

				info[registry.sId] = registry;
			}
		}

		{
			string sSelectSql = "select * from t_base_image";

			TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSelectSql);

			for (size_t i = 0; i < res.size(); i++)
			{
				BaseImageInfo baseImage;
				baseImage.id = res[i]["id"];
				baseImage.image = res[i]["image"];
				baseImage.registryId = res[i]["registryId"];
				baseImage.sha = res[i]["sha"];

				auto it = info.find(baseImage.registryId);
				if(it != info.end())
				{
					it->second.baseImages.push_back(baseImage);
				}
			}
		}
		return 0;
	}
	catch(exception &ex)
	{
		TLOGEX_ERROR("DockerThread","error:" << ex.what() << endl);
		return -1;
	}
}

int CDbHandle::updateBaseImageSha(const string &baseImageId, const string &sha)
{
	try
	{
		map<string, pair<TC_Mysql::FT, string> > mpColumns;
		mpColumns["sha"] = make_pair(TC_Mysql::FT::DB_STR, sha);

		_mysqlReg.updateRecord("t_base_image", mpColumns, "where id = '" + _mysqlReg.realEscapeString(baseImageId) + "'");
		return 0;
	}
	catch(exception &ex)
	{
		TLOGEX_ERROR("DockerThread", "error:" << ex.what() << endl);
		return -1;
	}
}

int CDbHandle::updateBaseImageResult(const string &baseImageId, const string &result)
{
	try
	{
		map<string, pair<TC_Mysql::FT, string> > mpColumns;
		mpColumns["result"] = make_pair(TC_Mysql::FT::DB_STR, result);

		_mysqlReg.updateRecord("t_base_image", mpColumns, "where id = '" + _mysqlReg.realEscapeString(baseImageId) + "'");
		return 0;
	}
	catch(exception &ex)
	{
		TLOGEX_ERROR("DockerThread", "error:" << ex.what() << endl);
		return -1;
	}
}


int CDbHandle::registerNode(const string& name, const NodeInfo& ni, const LoadInfo& li)
{
    try
    {
        string sSelectSql = "select present_state, node_obj, template_name "
                            "from t_node_info "
                            "where node_name='" + _mysqlReg.escapeString(name) + "'";

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSelectSql);
        TLOG_DEBUG("CDbHandle::init select node affected:" << res.size() << endl);

        string sTemplateName;
        if (res.size() != 0)
        {
            //合法性判断，是否存在名字相同已经存活但node obj不同的注册节点
            if (res[0]["present_state"] == "active" && !ni.nodeObj.empty() && res[0]["node_obj"] != ni.nodeObj)
            {
                TLOG_ERROR("registery node :" << name << " error, other node has registered, obj:" << res[0]["node_obj"] << ", register obj:" << ni.nodeObj << endl);
                return 1;
            }
            //传递已配置的模板名
            sTemplateName = res[0]["template_name"];
        }

        string sSql = "replace into t_node_info "
                      "    (node_name, node_obj, endpoint_ip, endpoint_port, data_dir, load_avg1, load_avg5, load_avg15,"
                      "     last_reg_time, last_heartbeat, setting_state, present_state, tars_version, template_name)"
                      "values('" + _mysqlReg.escapeString(name) + "', '" + _mysqlReg.escapeString(ni.nodeObj) + "', "
                      "    '" + _mysqlReg.escapeString(ni.endpointIp) + "',"
                      "    '" + TC_Common::tostr<int>(ni.endpointPort) + "',"
                      "    '" + _mysqlReg.escapeString(ni.dataDir) + "','" + TC_Common::tostr<float>(li.avg1) + "',"
                      "    '" + TC_Common::tostr<float>(li.avg5) + "',"
                      "    '" + TC_Common::tostr<float>(li.avg15) + "', now(), now(), 'active', 'active', " +
                      "    '" + _mysqlReg.escapeString(ni.version) + "', '" + _mysqlReg.escapeString(sTemplateName) + "')";

        _mysqlReg.execute(sSql);

        TLOG_DEBUG("registry node :" << name << " affected:" << _mysqlReg.getAffectedRows() << endl);

        NodeManager::getInstance()->createNodePrx(name, ni.nodeObj);
//
//        NodePrx nodePrx;
//        g_app.getCommunicator()->stringToProxy(ni.nodeObj, nodePrx);
//
//        TC_ThreadLock::Lock lock(_NodePrxLock);
//        _mapNodePrxCache[name] = nodePrx;

    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::init " << name << " exception: " << ex.what() << endl);
        return 2;
    }
    catch (TarsException& ex)
    {
        TLOG_ERROR("CDbHandle::init " << name << " exception: " << ex.what() << endl);
        return 3;
    }

    return 0;
}

int CDbHandle::destroyNode(const string& name)
{
    try
    {
        string sSql = "update t_node_info as node "
                      "left join t_server_conf as server using (node_name) "
                      "set node.present_state = 'inactive', server.present_state='inactive' "
                      "where node.node_name='" + _mysqlReg.escapeString(name) + "'";

        _mysqlReg.execute(sSql);

        TLOG_DEBUG("CDbHandle::destroyNode " << name << " affected:" << _mysqlReg.getAffectedRows() << endl);

        NodeManager::getInstance()->eraseNodePrx(name);

//        TC_ThreadLock::Lock lock(_NodePrxLock);
//        _mapNodePrxCache.erase(name);
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::destroyNode " << name << " exception: " << ex.what() << endl);
    }

    return 0;
}


int CDbHandle::keepAlive(const string& name, const LoadInfo& li)
{
    try
    {
        int64_t iStart = TNOWMS;
        string sSql = "update t_node_info "
                      "set last_heartbeat=now(), present_state='active',"
                      "    load_avg1=" + TC_Common::tostr<float>(li.avg1) + ","
                      "    load_avg5=" + TC_Common::tostr<float>(li.avg5) + ","
                      "    load_avg15=" + TC_Common::tostr<float>(li.avg15) + " "
                      "where node_name='" + _mysqlReg.escapeString(name) + "'";

        _mysqlReg.execute(sSql);

        TLOG_DEBUG("CDbHandle::keepAlive " << name << " affected:" << _mysqlReg.getAffectedRows() << "|cost:" << (TNOWMS - iStart) << endl);

        if (_mysqlReg.getAffectedRows() == 0)
        {
            return 1;
        }
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::keepAlive " << name << " exception: " << ex.what() << endl);
        return 2;
    }

    return 0;
}

map<string, string> CDbHandle::getActiveNodeList(string& result)
{
    map<string, string> mapNodeList;
    try
    {
        string sSql = "select node_name, node_obj from t_node_info "
                      "where present_state='active'";

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);

        TLOG_DEBUG("CDbHandle::getActiveNodeList (present_state='active') affected:" << res.size() << endl);

        for (unsigned i = 0; i < res.size(); i++)
        {
            mapNodeList[res[i]["node_name"]] = res[i]["node_obj"];
        }
    }
    catch (TC_Mysql_Exception& ex)
    {
        result = string(__FUNCTION__) + " exception: " + ex.what();
        TLOG_ERROR(result << endl);
        return mapNodeList;
    }

    return  mapNodeList;
}

int CDbHandle::getNodeVersion(const string& nodeName, string& version, string& result)
{
    try
    {
        string sSql = "select tars_version from t_node_info "
                      "where node_name='" + _mysqlReg.escapeString(nodeName) + "'";

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);

        TLOG_DEBUG("CDbHandle::getNodeVersion (node_name='" << nodeName << "') affected:" << res.size() << endl);

        if (res.size() > 0)
        {
            version = res[0]["tars_version"];
            return 0;

        }
        result = "node_name(" + nodeName + ") int table t_node_info not exist";
    }
    catch (TC_Mysql_Exception& ex)
    {

        TLOG_ERROR("CDbHandle::getNodeVersion exception:"<<ex.what());
    }
    return  -1;
}

vector<ServerDescriptor> CDbHandle::getServers(const string& app, const string& serverName, const string& nodeName, bool withDnsServer)
{
    string sSql;
    vector<ServerDescriptor>  vServers;
    unsigned num = 0;
    int64_t iStart = TNOWMS;

    try
    {
        //server详细配置
        string sCondition;
        sCondition += "server.node_name='" + _mysqlReg.escapeString(nodeName) + "'";
        if (app != "")        sCondition += " and server.application='" + _mysqlReg.escapeString(app) + "' ";
        if (serverName != "") sCondition += " and server.server_name='" + _mysqlReg.escapeString(serverName) + "' ";
        if (withDnsServer == false) sCondition += " and server.server_type !='tars_dns' "; //不获取dns服务

        sSql = "select server.application, server.server_name, server.node_name, run_type, base_image_id, base_path, "
               "    exe_path, setting_state, present_state, adapter_name, thread_num, async_thread_num, endpoint,"
               "    profile,template_name, "
               "    allow_ip, max_connections, servant, queuecap, queuetimeout,protocol,handlegroup,"
               "    patch_version, patch_time, patch_user, "
               "    server_type, start_script_path, stop_script_path, monitor_script_path,config_center_port ,"
               "    enable_set, set_name, set_area, set_group, bak_flag "
               "from t_server_conf as server "
               "    left join t_adapter_conf as adapter using(application, server_name, node_name) "
               "where " + sCondition;


        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);
        num = res.size();

        TLOG_DEBUG("CDbHandle::getServers sSql:" << sSql << ", num:" << num << ", cost:" << TNOWMS - iStart << endl);

        //对应server在vector的下标
        map<string, int> mapAppServerTemp;

        //获取模版profile内容
        map<string, string> mapProfile;

        //分拆数据到server的信息结构里
        for (unsigned i = 0; i < res.size(); i++)
        {
            string sServerId = res[i]["application"] + "." + res[i]["server_name"]
                               + "_" + res[i]["node_name"];

            if (mapAppServerTemp.find(sServerId) == mapAppServerTemp.end())
            {
                //server配置
                ServerDescriptor server;
                server.application  = res[i]["application"];
                server.serverName   = res[i]["server_name"];
                server.nodeName     = res[i]["node_name"];
                server.basePath     = res[i]["base_path"];
                server.exePath      = res[i]["exe_path"];
                server.settingState = res[i]["setting_state"];
                server.presentState = res[i]["present_state"];
                server.patchVersion = res[i]["patch_version"];
                server.patchTime    = res[i]["patch_time"];
                server.patchUser    = res[i]["patch_user"];
                server.profile      = res[i]["profile"];
                server.serverType   = res[i]["server_type"];
                server.startScript  = res[i]["start_script_path"];
                server.stopScript   = res[i]["stop_script_path"];
                server.monitorScript    = res[i]["monitor_script_path"];
                server.configCenterPort = TC_Common::strto<int>(res[i]["config_center_port"]);
				server.runType      = res[i]["run_type"];
				server.baseImageId 	= res[i]["base_image_id"];
//				server.bakFlag      = TC_Common::strto<int>(res[i]["bak_flag"]);

				if(!server.baseImageId.empty())
				{
					auto image = g_app.getDockerThread()->getBaseImageById(server.baseImageId);
					server.baseImage = image.first;
					server.sha = image.second;
				}

                server.setId = "";
                if (TC_Common::lower(res[i]["enable_set"]) == "y")
                {
                    server.setId = res[i]["set_name"] + "." +  res[i]["set_area"] + "." + res[i]["set_group"];
                }

				TLOG_DEBUG("CDbHandle::getServers begin getProfileTemplate" << endl);

				//获取父模版profile内容
                if (mapProfile.find(res[i]["template_name"]) == mapProfile.end())
                {
                    string sResult;
                    mapProfile[res[i]["template_name"]] = getProfileTemplate(res[i]["template_name"], sResult);
                }
				TLOG_DEBUG("CDbHandle::getServers begin getProfileTemplate cost:" << TNOWMS - iStart << endl);

                TC_Config tParent, tProfile;
                tParent.parseString(mapProfile[res[i]["template_name"]]);
                tProfile.parseString(server.profile);

                int iDefaultAsyncThreadNum = 3;

                if("tars_nodejs" == server.serverType) 
                { 
                    //tars_nodejs类型的业务需要设置这个值为0
                    iDefaultAsyncThreadNum = 0; 
                }

                int iConfigAsyncThreadNum = TC_Common::strto<int>(TC_Common::trim(res[i]["async_thread_num"]));
                iDefaultAsyncThreadNum = iConfigAsyncThreadNum > iDefaultAsyncThreadNum ? iConfigAsyncThreadNum : iDefaultAsyncThreadNum;
                server.asyncThreadNum = TC_Common::strto<int>(tProfile.get("/tars/application/client<asyncthread>", TC_Common::tostr(iDefaultAsyncThreadNum)));
                tParent.joinConfig(tProfile, true);
                server.profile = tParent.tostr();

                mapAppServerTemp[sServerId] = vServers.size();
                vServers.push_back(server);
            }

            //adapter配置
            AdapterDescriptor adapter;
            adapter.adapterName = res[i]["adapter_name"];
            if (adapter.adapterName == "")
            {
                //adapter没配置，left join 后为 NULL,不放到adapters map
                continue;
            }

            adapter.threadNum       = res[i]["thread_num"];
            adapter.endpoint        = res[i]["endpoint"];
            adapter.maxConnections  = TC_Common::strto<int>(res[i]["max_connections"]);
            adapter.allowIp         = res[i]["allow_ip"];
            adapter.servant         = res[i]["servant"];
            adapter.queuecap        = TC_Common::strto<int>(res[i]["queuecap"]);
            adapter.queuetimeout    = TC_Common::strto<int>(res[i]["queuetimeout"]);
            adapter.protocol        = res[i]["protocol"];
            adapter.handlegroup     = res[i]["handlegroup"];

            vServers[mapAppServerTemp[sServerId]].adapters[adapter.adapterName] = adapter;
        }
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::getServers " << app << "." << serverName << "_" << nodeName
                  << " exception: " << ex.what() << "|" << sSql << endl);
        return vServers;
    }
    catch (TC_Config_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::getServers " << app << "." << serverName << "_" << nodeName
                  << " TC_Config_Exception exception: " << ex.what() << endl);
        throw TarsException(string("TC_Config_Exception exception: ") + ex.what());
    }

    TLOG_DEBUG(app << "." << serverName << "_" << nodeName << " getServers affected:" << num
              << "|cost:" << (TNOWMS - iStart) << endl);

    return  vServers;

}

string CDbHandle::getProfileTemplate(const string& sTemplateName, string& sResultDesc)
{
    map<string, int> mapRecursion;
    return getProfileTemplate(sTemplateName, mapRecursion, sResultDesc);
}

void CDbHandle::getAllDynamicWeightServant(std::vector<string> &vtServant)
{
    ostringstream log;
    const auto &objectCache = _objectsCache.getReaderData();
    log << objectCache.size() << "|";
    for (const auto &objPair : objectCache)
    {
        if (!objPair.second.vActiveEndpoints.empty())
        {
            if (LOAD_BALANCE_DYNAMIC_WEIGHT == objPair.second.vActiveEndpoints[0].weightType)
            {
                vtServant.push_back(objPair.first);
                log << objPair.first << "; ";
            }
        }
    }

    log << "|vtServant size: " << vtServant.size();
    TLOG_DEBUG(log.str() << "|" << endl);
}

string CDbHandle::getProfileTemplate(const string& sTemplateName, map<string, int>& mapRecursion, string& sResultDesc)
{
    try
    {
        string sSql = "select template_name, parents_name, profile from t_profile_template "
                      "where template_name='" + _mysqlReg.escapeString(sTemplateName) + "'";

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);

        if (res.size() == 0)
        {
            sResultDesc += "(" + sTemplateName + ":template not found)";
            return "";
        }

        TC_Config confMyself, confParents;
        confMyself.parseString(res[0]["profile"]);
        //mapRecursion用于避免重复继承
        mapRecursion[res[0]["template_name"]] = 1;

        if (res[0]["parents_name"] != "" && mapRecursion.find(res[0]["parents_name"]) == mapRecursion.end())
        {
            confParents.parseString(getProfileTemplate(res[0]["parents_name"], mapRecursion, sResultDesc));
            confMyself.joinConfig(confParents, false);
        }
        sResultDesc += "(" + sTemplateName + ":OK)";

        TLOG_DEBUG("CDbHandle::getProfileTemplate " << sTemplateName << " " << sResultDesc << endl);

        return confMyself.tostr();
    }
    catch (TC_Mysql_Exception& ex)
    {
        sResultDesc += "(" + sTemplateName + ":" + ex.what() + ")";
        TLOG_ERROR("CDbHandle::getProfileTemplate exception: " << ex.what() << endl);
    }
    catch (TC_Config_Exception& ex)
    {
        sResultDesc += "(" + sTemplateName + ":" + ex.what() + ")";
        TLOG_ERROR("CDbHandle::getProfileTemplate TC_Config_Exception exception: " << ex.what() << endl);
    }

    return  "";
}

vector<string> CDbHandle::getAllApplicationNames(string& result)
{
    vector<string> vApps;
    try
    {
        string sSql = "select distinct application from t_server_conf";

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);
        TLOG_DEBUG("CDbHandle::getAllApplicationNames affected:" << res.size() << endl);

        for (unsigned i = 0; i < res.size(); i++)
        {
            vApps.push_back(res[i]["application"]);
        }
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::getAllApplicationNames exception: " << ex.what() << endl);
        return vApps;
    }

    return vApps;
}

vector<vector<string> > CDbHandle::getAllServerIds(string& result)
{
    vector<vector<string> > vServers;
    try
    {
        string sSql = "select application, server_name, node_name, setting_state, present_state,server_type from t_server_conf";

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);
        TLOG_DEBUG("CDbHandle::getAllServerIds affected:" << res.size() << endl);

        for (unsigned i = 0; i < res.size(); i++)
        {
            vector<string> server;
            server.push_back(res[i]["application"] + "." + res[i]["server_name"] +  "_" + res[i]["node_name"]);
            server.push_back(res[i]["setting_state"]);
            server.push_back(res[i]["present_state"]);
            server.push_back(res[i]["server_type"]);
            vServers.push_back(server);
        }
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::getAllServerIds exception: " << ex.what() << endl);
        return vServers;
    }

    return vServers;

}
int CDbHandle::updateServerState(const string& app, const string& serverName, const string& nodeName, const string& stateFields, ServerState state, int processId)
{
    try
    {
        int64_t iStart = TNOWMS;
        if (stateFields != "setting_state" && stateFields != "present_state")
        {
            TLOG_DEBUG(app << "." << serverName << "_" << nodeName
                      << " not supported fields:" << stateFields << endl);
            return -1;
        }

        string sProcessIdSql = (stateFields == "present_state" ?
                                (", process_id = " + TC_Common::tostr<int>(processId) + " ") : "");

        string sSql = "update t_server_conf "
                      "set " + stateFields + " = '" + etos(state) + "' " + sProcessIdSql +
                      "where application='" + _mysqlReg.escapeString(app) + "' "
                      "    and server_name='" + _mysqlReg.escapeString(serverName) + "' "
                      "    and node_name='" + _mysqlReg.escapeString(nodeName) + "' ";

        _mysqlReg.execute(sSql);
        TLOG_DEBUG("CDbHandle::updateServerState " << app << "." << serverName << "_" << nodeName
                  << " affected:" << _mysqlReg.getAffectedRows()
                  << "|cost:" << (TNOWMS - iStart) << endl);

        {
            TC_ThreadLock::Lock lock(_mapServantStatusLock);
            ServantStatusKey statusKey = { app, serverName, nodeName };
            _mapServantStatus[statusKey] = static_cast<int>(state);
        }
        return 0;

    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::updateServerState " << app << "." << serverName << "_" << nodeName
                  << " exception: " << ex.what() << endl);
        return -1;
    }
}

int CDbHandle::updateServerStateBatch(const std::vector<ServerStateInfo>& vecStateInfo)
{
    const size_t sizeStep = 1000;

    for (std::vector<ServerStateInfo>::size_type i = 0; i < vecStateInfo.size();)
    {
        int iEnd = (i + sizeStep >= vecStateInfo.size()) ? vecStateInfo.size() : (i + sizeStep);
        if (doUpdateServerStateBatch(vecStateInfo, i, iEnd) != 0) return -1;
        i = iEnd;
    }

    return 0;
}

int CDbHandle::doUpdateServerStateBatch(const std::vector<ServerStateInfo>& vecStateInfo, const size_t sizeBegin, const size_t sizeEnd)
{
    std::map<std::string, std::map<std::string, std::vector<int> > > map_sort;

    {
        TC_ThreadLock::Lock lock(_mapServantStatusLock);
        for (std::vector<ServerStateInfo>::size_type i = sizeBegin; i < sizeEnd; i++)
        {
            ServantStatusKey statusKey = { vecStateInfo[i].application, vecStateInfo[i].serverName, vecStateInfo[i].nodeName };
            std::map<ServantStatusKey, int>::iterator it = _mapServantStatus.find(statusKey);
            if (it != _mapServantStatus.end() && it->second == static_cast<int>(vecStateInfo[i].serverState)) continue;

            map_sort[vecStateInfo[i].application][vecStateInfo[i].serverName].push_back(i);
        }
    }

    if (map_sort.empty())
    {
        TLOG_DEBUG("CDbHandle::doUpdateServerStateBatch vector size(" << vecStateInfo.size() << ") do nothing within the same state in cache" << endl);
        return 0;
    }

    std::string sCommand;
    //新更新的状态，等语句更新成功后赋值
    std::map<ServantStatusKey, int> updated_map;
    try
    {
        int64_t iStart = TC_TimeProvider::getInstance()->getNowMs();

        if (_enMultiSql)
        {
            std::string sPrefix = "UPDATE t_server_conf SET present_state='";
            for (std::map<std::string, std::map<std::string, std::vector<int> > >::iterator it_app = map_sort.begin(); it_app != map_sort.end(); it_app++)
            {
                for (std::map<std::string, std::vector<int> >::iterator it_svr = it_app->second.begin(); it_svr != it_app->second.end(); it_svr++)
                {
                    for (std::vector<int>::size_type i = 0; i < it_svr->second.size(); i++)
                    {
                        sCommand += (sCommand == "" ? "" : ";") + sPrefix + etos(vecStateInfo[it_svr->second[i]].serverState)
                                    + "', process_id= " + TC_Common::tostr<int>(vecStateInfo[it_svr->second[i]].processId)
                                    + " WHERE application='" + _mysqlReg.escapeString(it_app->first)
                                    + "' AND server_name='" + _mysqlReg.escapeString(it_svr->first)
                                    + "' AND node_name='" + _mysqlReg.escapeString(vecStateInfo[it_svr->second[i]].nodeName) + "'";

                        ServantStatusKey statusKey = { it_app->first, it_svr->first, vecStateInfo[it_svr->second[i]].nodeName };
                        updated_map[statusKey] = vecStateInfo[it_svr->second[i]].serverState;
                    }
                }
            }
        }
        else
        {
            std::string sPidCommand, sPreCommand;
            for (std::map<std::string, std::map<std::string, std::vector<int> > >::iterator it_app = map_sort.begin(); it_app != map_sort.end(); it_app++)
            {
                sPidCommand += " WHEN '" + it_app->first + "' THEN CASE server_name ";
                sPreCommand += " WHEN '" + it_app->first + "' THEN CASE server_name ";

                for (std::map<std::string, std::vector<int> >::iterator it_svr = it_app->second.begin(); it_svr != it_app->second.end(); it_svr++)
                {
                    sPidCommand += " WHEN '" + it_svr->first + "' THEN CASE node_name ";
                    sPreCommand += " WHEN '" + it_svr->first + "' THEN CASE node_name ";

                    for (std::vector<int>::size_type i = 0; i < it_svr->second.size(); i++)
                    {
                        sPidCommand += " WHEN '" + _mysqlReg.escapeString(vecStateInfo[it_svr->second[i]].nodeName) + "' THEN " + TC_Common::tostr<int>(vecStateInfo[it_svr->second[i]].processId);
                        sPreCommand += " WHEN '" + _mysqlReg.escapeString(vecStateInfo[it_svr->second[i]].nodeName) + "' THEN '" + etos(vecStateInfo[it_svr->second[i]].serverState) + "'";

                        ServantStatusKey statusKey = { it_app->first, it_svr->first, vecStateInfo[it_svr->second[i]].nodeName };
                        updated_map[statusKey] = vecStateInfo[it_svr->second[i]].serverState;

                    }

                    sPidCommand += " ELSE process_id END";
                    sPreCommand += " ELSE present_state END";
                }

                sPidCommand += " ELSE process_id END";
                sPreCommand += " ELSE present_state END";
            }
            sCommand = "UPDATE t_server_conf SET process_id= CASE application " + sPidCommand + " ELSE process_id END, present_state= CASE application " + sPreCommand + " ELSE present_state END";
        }

        if (!sCommand.empty())
        {
            _mysqlReg.execute(sCommand);
            int iRows = 0;
            if (_enMultiSql)
            {
                for (iRows = mysql_affected_rows(_mysqlReg.getMysql()); !mysql_next_result(_mysqlReg.getMysql());
                     iRows += mysql_affected_rows(_mysqlReg.getMysql())) ;
            }
            else
            {
                iRows = mysql_affected_rows(_mysqlReg.getMysql());
            }

            if (iRows > 0)
            {
                TLOG_DEBUG("CDbHandle::doUpdateServerStateBatch sql: " << sCommand << " vector:" << vecStateInfo.size() << " affected:" << iRows
                          << "|cost:" << (TNOWMS - iStart) << endl);

                TC_ThreadLock::Lock lock(_mapServantStatusLock);
                //insert will fail when map has same key
                //_mapServantStatus.insert(updated_map.begin(), updated_map.end());
                for(auto &kv : updated_map)
                {
                    _mapServantStatus[kv.first] = kv.second;
                }
            }
        }
        return 0;
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::doUpdateServerStateBatch exception: " << ex.what() << " sql:" << sCommand << endl);
        return -1;
    }
    catch (exception& ex)
    {
        TLOG_ERROR("CDbHandle::doUpdateServerStateBatch " << ex.what() << endl);
        return -1;
    }
    return -1;
}

int CDbHandle::setPatchInfo(const string& app, const string& serverName, const string& nodeName, const string& version, const string& user)
{
    try
    {
        string sSql = "update t_server_conf "
                      "set patch_version = '" + _mysqlReg.escapeString(version) + "', "
                      "   patch_user = '" + _mysqlReg.escapeString(user) + "', "
                      "   patch_time = now() "
                      "where application='" + _mysqlReg.escapeString(app) + "' "
                      "    and server_name='" + _mysqlReg.escapeString(serverName) + "' "
                      "    and node_name='" + _mysqlReg.escapeString(nodeName) + "' ";

        _mysqlReg.execute(sSql);

        TLOG_DEBUG("CDbHandle::setPatchInfo " << app << "." << serverName << "_" << nodeName << " affected:" << _mysqlReg.getAffectedRows() << endl);

        return 0;
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::setPatchInfo " << app << "." << serverName << "_" << nodeName << " exception: " << ex.what() << endl);
        return -1;
    }
}

int CDbHandle::setServerTarsVersion(const string& app, const string& serverName, const string& nodeName, const string& version)
{
    try
    {

        int64_t iStart = TNOWMS;
        string sSql = "update t_server_conf "
                      "set tars_version = '" + _mysqlReg.escapeString(version) + "' "
                      "where application='" + _mysqlReg.escapeString(app) + "' "
                      "    and server_name='" + _mysqlReg.escapeString(serverName) + "' "
                      "    and node_name='" + _mysqlReg.escapeString(nodeName) + "' ";

        _mysqlReg.execute(sSql);

        TLOG_DEBUG("CDbHandle::setServerTarsVersion " << app << "." << serverName << "_" << nodeName
                  << " affected:" << _mysqlReg.getAffectedRows()
                  << "|cost:" << (TNOWMS - iStart) << endl);

        return 0;
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::setServerTarsVersion " << app << "." << serverName << "_" << nodeName << " exception: " << ex.what() << endl);
        return -1;
    }
}

NodePrx CDbHandle::getNodePrx(const string& nodeName)
{
    try
    {
        auto nodePrx = NodeManager::getInstance()->getNodePrx(nodeName);

        if(nodePrx)
        {
            return nodePrx;
        }

        string sSql = "select node_obj "
                      "from t_node_info "
                      "where node_name='" + _mysqlReg.escapeString(nodeName) + "' and present_state='active'";

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);
        TLOG_DEBUG("CDbHandle::getNodePrx '" << nodeName << "' affected:" << res.size() << endl);

        if (res.size() == 0)
        {
            throw Tars("node '" + nodeName + "' not registered  or heartbeart timeout,please check for it");
        }

        nodePrx = NodeManager::getInstance()->createNodePrx(nodeName, res[0]["node_obj"]);

        return nodePrx;

    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::getNodePrx " << nodeName << " exception: " << ex.what() << endl);
        throw Tars(string("get node record from db error:") + ex.what());
    }
    catch (TarsException& ex)
    {
        TLOG_ERROR("CDbHandle::getNodePrx " << nodeName << " exception: " << ex.what() << endl);
        throw ex;
    }

}

int CDbHandle::checkNodeTimeout(unsigned uTimeout)
{
    try
    {
        //这里先检查下，记录下有哪些节点超时了，方便定位问题
        {
            string sTmpSql = "select node_name from t_node_info where last_heartbeat < date_sub(now(), INTERVAL " + TC_Common::tostr(uTimeout) + " SECOND)";
            TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sTmpSql);
            if (res.size() > 0)
            {
                TLOG_DEBUG( "CDbHandle::checkNodeTimeout affected:" << TC_Common::tostr(res.data()) << endl);
            }
        }

        int64_t iStart = TNOWMS;

        string sSql = "update t_node_info as node "
                      "    left join t_server_conf as server using (node_name) "
                      "set node.present_state='inactive', server.present_state='inactive', server.process_id=0 "
                      "where last_heartbeat < date_sub(now(), INTERVAL " + TC_Common::tostr(uTimeout) + " SECOND)";

        _mysqlReg.execute(sSql);

        TLOG_DEBUG("CDbHandle::checkNodeTimeout (" << uTimeout  << "s) affected:" << _mysqlReg.getAffectedRows() << "|cost:" << (TNOWMS - iStart) << endl);

        return _mysqlReg.getAffectedRows();

    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::checkNodeTimeout exception: " << ex.what() << endl);
        return -1;
    }

}

int CDbHandle::checkRegistryTimeout(unsigned uTimeout)
{
    try
    {
        string sSql = "update t_registry_info "
                      "set present_state='inactive' "
                      "where last_heartbeat < date_sub(now(), INTERVAL " + TC_Common::tostr(uTimeout) + " SECOND)";

        _mysqlReg.execute(sSql);

        TLOG_DEBUG("CDbHandle::checkRegistryTimeout (" << uTimeout  << "s) affected:" << _mysqlReg.getAffectedRows() << endl);

        return _mysqlReg.getAffectedRows();

    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::checkRegistryTimeout exception: " << ex.what() << endl);
        return -1;
    }

}
//
//int CDbHandle::checkSettingState(const int iCheckLeastChangedTime)
//{
//    try
//    {
//        TLOG_DEBUG("CDbHandle::checkSettingState ____________________________________" << endl);
//
//        string sSql = "select application, server_name, node_name, setting_state "
//                      "from t_server_conf "
//                      "where setting_state='active' "  //检查应当启动的
//                      "and server_type != 'tars_dns'"  //仅用来提供dns服务的除外
//                      "and registry_timestamp >='" + TC_Common::tm2str(TC_TimeProvider::getInstance()->getNow() - iCheckLeastChangedTime) + "'";
//
//        int64_t iStart = TNOWMS;
//
//        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);
//
//        TLOG_DEBUG("CDbHandle::checkSettingState setting_state='active' affected:" << res.size() << "|cost:" << (TNOWMS - iStart) << endl);
//
////        NodePrx nodePrx;
//        for (unsigned i = 0; i < res.size(); i++)
//        {
//            string sResult;
//            TLOG_DEBUG("checking [" << i << "]: " << res[i]["application"] << "." << res[i]["server_name"] << "_" << res[i]["node_name"] << endl);
//            try
//            {
//				NodePrx nodePrx = getNodePrx(res[i]["node_name"]);
//                if (nodePrx)
//                {
//                    try
//                    {
//                        if (nodePrx->getSettingState(res[i]["application"], res[i]["server_name"], sResult) != Active)
//                        {
//                            string sTempSql = "select application, server_name, node_name, setting_state "
//                                              "from t_server_conf "
//                                              "where setting_state='active' "
//                                              "and application = '" + res[i]["application"] + "' "
//                                                                                              "and server_name = '" +
//                                              res[i]["server_name"] + "' "
//                                                                      "and node_name = '" + res[i]["node_name"] + "'";
//
//                            if (_mysqlReg.queryRecord(sTempSql).size() == 0)
//                            {
//                                TLOG_DEBUG(res[i]["application"] << "." << res[i]["server_name"] << "_"
//                                                                 << res[i]["node_name"]
//                                                                 << " not setting active,and not need restart" << endl);
//                                continue;
//                            }
//
//                            TLOG_DEBUG(
//                                    res[i]["application"] << "." << res[i]["server_name"] << "_" << res[i]["node_name"]
//                                                          << " not setting active, start it" << endl);
//
//                            int iRet = nodePrx->startServer(res[i]["application"], res[i]["server_name"], sResult);
//
//                            TLOG_DEBUG("startServer ret=" << iRet << ",result=" << sResult << endl);
//                        }
//                    }
//                    catch(exception &ex)
//                    {
//                        TLOG_ERROR("checking " << res[i]["application"] << "." << res[i]["server_name"] << "_" << res[i]["node_name"]
//                                               << "' exception: " << ex.what() << endl);
//                    }
//                }
//            }
//
//            catch (TarsException& ex)
//            {
//                TLOG_ERROR("checking " << res[i]["application"] << "." << res[i]["server_name"] << "_" << res[i]["node_name"]
//                          << "' exception: " << ex.what() << endl);
//            }
//            catch (exception& ex)
//            {
//                TLOG_ERROR("checking " << res[i]["application"] << "." << res[i]["server_name"] << "_" << res[i]["node_name"] << "' exception: " << ex.what() << endl);
//            }
//        }
//    }
//    catch (TC_Mysql_Exception& ex)
//    {
//        TLOG_ERROR("CDbHandle::checkSettingState  exception: " << ex.what() << endl);
//        return -1;
//    }
//    TLOG_DEBUG("CDbHandle::checkSettingState ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" << endl);
//
//    return 0;
//}

//
//int CDbHandle::checkSettingState(const int iCheckLeastChangedTime)
//{
//    try
//    {
//        TLOG_DEBUG("CDbHandle::checkSettingState ____________________________________" << endl);
//
//        string sSql = "select application, server_name, node_name, setting_state "
//                      "from t_server_conf "
//                      "where setting_state='active' "  //检查应当启动的
//                      "and server_type != 'tars_dns'"  //仅用来提供dns服务的除外
//                      "and registry_timestamp >='" + TC_Common::tm2str(TC_TimeProvider::getInstance()->getNow() - iCheckLeastChangedTime) + "'";
//
//        int64_t iStart = TNOWMS;
//
//        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);
//
//        TLOG_DEBUG("CDbHandle::checkSettingState setting_state='active' affected:" << res.size() << "|cost:" << (TNOWMS - iStart) << endl);
//
//        for (unsigned i = 0; i < res.size(); i++)
//        {
//            TLOG_DEBUG("checking [" << i << "]: " << res[i]["application"] << "." << res[i]["server_name"] << "_" << res[i]["node_name"] << endl);
//            try
//            {
//              	NodeManager::getInstance()->async_startServer(res[i]["application"], res[i]["server_name"], res[i]["node_name"]);
//            }
//            catch (TarsException& ex)
//            {
//                TLOG_ERROR("checking " << res[i]["application"] << "." << res[i]["server_name"] << "_" << res[i]["node_name"]
//                                       << "' exception: " << ex.what() << endl);
//            }
//            catch (exception& ex)
//            {
//                TLOG_ERROR("checking " << res[i]["application"] << "." << res[i]["server_name"] << "_" << res[i]["node_name"] << "' exception: " << ex.what() << endl);
//            }
//        }
//    }
//    catch (TC_Mysql_Exception& ex)
//    {
//        TLOG_ERROR("CDbHandle::checkSettingState  exception: " << ex.what() << endl);
//        return -1;
//    }
//    TLOG_DEBUG("CDbHandle::checkSettingState ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" << endl);
//
//    return 0;
//}


int CDbHandle::getGroupId(const string& ip)
{

    map<string, int>& groupIdMap = _groupIdMap.getReaderData();
    map<string, int>::iterator it = groupIdMap.find(ip);
    if (it != groupIdMap.end())
    {
        return it->second;
    }

    uint32_t uip = stringIpToInt(ip);
    string ipStar = Ip2StarStr(uip);
    it = groupIdMap.find(ipStar);
    if (it != groupIdMap.end())
    {
        return it->second;
    }

    return -1;
}

int CDbHandle::getGroupIdByName(const string& sGroupName)
{
    int iGroupId = -1;
    try
    {
        if (sGroupName.empty())
        {
            return iGroupId;
        }

        map<string, int>& groupNameMap = _groupNameMap.getReaderData();
        map<string, int>::iterator it = groupNameMap.find(sGroupName);
        if (it != groupNameMap.end())
        {
            TLOGINFO("CDbHandle::getGroupIdByName: "<< sGroupName << "|" << it->second << endl);
            return it->second;
        }
    }
    catch (exception& ex)
    {
        TLOG_ERROR("CDbHandle::getGroupIdByName exception:" << ex.what() << endl);
    }
    catch (...)
    {
        TLOG_ERROR("CDbHandle::getGroupIdByName unknown exception" << endl);
    }

    TLOGINFO("CDbHandle::getGroupIdByName " << sGroupName << "|" << endl);
    return -1;
}

int CDbHandle::loadIPPhysicalGroupInfo(bool fromInit)
{
    TC_Mysql::MysqlData res;
    try
    {
        string sSql = "select group_id,ip_order,allow_ip_rule,denny_ip_rule,group_name from t_server_group_rule "
                      "order by group_id";

        res = _mysqlReg.queryRecord(sSql);

        TLOG_DEBUG("CDbHandle::loadIPPhysicalGroupInfo get server group from db, records affected:" << res.size() << endl);

        load2GroupMap(res.data());
    }
    catch (TC_Mysql_Exception& ex)
    {
        sendSqlErrorAlarmSMS(string("CDbHandle::loadIPPhysicalGroupInfo:") + ex.what());
        TLOG_ERROR("CDbHandle::loadIPPhysicalGroupInfo exception: " << ex.what() << endl);
        if (fromInit)
        {
            //初始化是出现异常，退出, 八成是数据库权限问题
            assert(0);
        }
    }
    catch (exception& ex)
    {
	    sendSqlErrorAlarmSMS(string("CDbHandle::loadIPPhysicalGroupInfo:") + ex.what());
        TLOG_DEBUG("CDbHandle::loadIPPhysicalGroupInfo " << ex.what() << endl);
        if (fromInit)
        {
            assert(0);
        }
    }
    return -1;
}

void CDbHandle::load2GroupMap(const vector<map<string, string> >& serverGroupRule)
{
    map<string, int>& groupIdMap = _groupIdMap.getWriterData();
    map<string, int>& groupNameMap = _groupNameMap.getWriterData();
    groupIdMap.clear();  //规则改变 清除以前缓存
    groupNameMap.clear();
    vector<map<string, string> >::const_iterator it = serverGroupRule.begin();
    for (; it != serverGroupRule.end(); it++)
    {
        int groupId = TC_Common::strto<int>(it->find("group_id")->second);
        vector<string> vIp = TC_Common::sepstr<string>(it->find("allow_ip_rule")->second, "|");
        for (size_t j = 0; j < vIp.size(); j++)
        {
            groupIdMap[vIp[j]] = groupId;
        }

        groupNameMap[it->find("group_name")->second] = groupId;
    }
    _groupIdMap.swap();
    _groupNameMap.swap();

}


int CDbHandle::loadGroupPriority(bool fromInit)
{
    std::map<int, GroupPriorityEntry> & mapPriority = _mapGroupPriority.getWriterData();
    mapPriority.clear();
    try
    {
        std::string s_command("SELECT id,group_list,station FROM t_group_priority ORDER BY list_order ASC");

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(s_command);
        TLOG_DEBUG("CDbHandle::loadGroupPriority load group priority from db, records affected:" << res.size() << endl);

        for (unsigned int i = 0; i < res.size(); i++)
        {
            mapPriority[i].sGroupID = res[i]["id"];
            mapPriority[i].sStation = res[i]["station"];

            std::vector<int> vecGroupID = TC_Common::sepstr<int>(res[i]["group_list"], "|,;", false);
            std::copy(vecGroupID.begin(), vecGroupID.end(), std::inserter(mapPriority[i].setGroupID, mapPriority[i].setGroupID.begin()));
            TLOG_DEBUG("loaded groups priority to cache [" << mapPriority[i].sStation << "] group size:" << mapPriority[i].setGroupID.size() << endl);
        }

        _mapGroupPriority.swap();

        TLOG_DEBUG("loaded groups priority to cache virtual group size:" << mapPriority.size() << endl);
    }
    catch (TC_Mysql_Exception& ex)
    {
        sendSqlErrorAlarmSMS(string("CDbHandle::loadGroupPriority:") + ex.what());
        TLOG_ERROR("CDbHandle::loadGroupPriority exception: " << ex.what() << endl);
        if (fromInit)
        {
            assert(0);
        }
        return -1;
    }
    catch (exception& ex)
    {
	    sendSqlErrorAlarmSMS(string("CDbHandle::loadGroupPriority:") + ex.what());
        TLOG_DEBUG("CDbHandle::loadGroupPriority " << ex.what() << endl);
        if (fromInit)
        {
            assert(0);
        }
        return -1;
    }

    return 0;
}

int CDbHandle::loadStatData(const vector<string> &vtServer, tars::TC_Mysql::MysqlData &statData)
{
    if (vtServer.empty())
    {
        TLOG_ERROR("CDbHandle::loadStatData empty vtServer!!!" << endl);

        return LOAD_BALANCE_DB_EMPTY_LIST;
    }

    auto now(TNOW);
    auto dateHour(TC_Common::tm2str(now, "%Y%m%d%H"));
    auto date(TC_Common::tm2str(now, "%Y%m%d"));
    ostringstream sql;
    /*sql << "select stattime, f_date, slave_name, slave_ip, succ_count, timeout_count, ave_time from tars_stat_" << dateHour
        << " where f_date = '" << date
        << "' and stattime >= DATE_SUB(NOW(), INTERVAL 5 MINUTE) and slave_name in(";

    auto SIZE{vtServer.size()};
    decltype(SIZE) count{0};
    for (const string &server : vtServer)
    {
        ++count;
        sql << "'" << server << "'" << (count < SIZE ? ", " : "");
    }

    sql << ")";*/

    sql << "select stattime, f_date, slave_name, slave_ip, succ_count, timeout_count, ave_time from tars_stat_" << dateHour
        << " where f_date = '" << date
        << "' and stattime >= DATE_SUB(NOW(), INTERVAL 5 MINUTE) and slave_name regexp '";

    auto SIZE(vtServer.size());
    decltype(SIZE) count(0);
    for (const string &server : vtServer)
    {
        ++count;
        auto pos(server.find_last_of(static_cast<string>(".")));
        if (string::npos != pos)
        {
            sql << server.substr(0, pos) << (count < SIZE ? "|" : "");
        }
    }

    sql << "'";

    auto getData = [] (tars::TC_Mysql::MysqlData &statData, const string &sql) {
        try
        {
            statData = _mysqlQueryStat.queryRecord(sql);
        }
        catch (TC_Mysql_Exception& ex)
        {
            TLOG_ERROR("CDbHandle::loadStatData exception: " << ex.what() << endl);

            return LOAD_BALANCE_DB_FAILED;
        }
        catch (exception& ex)
        {
            TLOG_ERROR("CDbHandle::loadStatData " << ex.what() << endl);

            return LOAD_BALANCE_DB_FAILED;
        }

        return LOAD_BALANCE_DB_SUCCESS;
    };

    int ret(getData(statData, sql.str()));
    if (LOAD_BALANCE_DB_SUCCESS == ret && !statData.size())
    {
        // 跨小时的情况下可能查不到整点的数据，返回上个表再查一次
        auto query(TC_Common::replace(sql.str(), dateHour, TC_Common::tm2str(now - 3600, "%Y%m%d%H")));
        ret = getData(statData, query);
    }

    ostringstream log;
    log << "vtServer size: " << SIZE << "|"
        << "now: " << now << "|"
        << "dateHour: " << dateHour << "|"
        << "date: " << date << "|"
        << "sql: " << sql.str() << "|"
        << "statData size: " << statData.size() << "|";

    TLOG_DEBUG(log.str() << endl);

    return ret;
}

int CDbHandle::computeInactiveRate()
{
    try
    {
        std::string sCommand("SELECT SUM(CASE present_state WHEN 'active' THEN 1 ELSE 0 END) AS active, "
                             "SUM(CASE present_state WHEN 'inactive' THEN 1 ELSE 0 END) AS inactive FROM t_node_info;");

        int64_t iStart = TNOWMS;
        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sCommand);
        TLOGINFO(__FUNCTION__ << "|cost:" << (TNOWMS - iStart) << endl);
        if (res.size() != 1)
        {
            TLOG_DEBUG("loaded inactive rate failed, size(" << res.size() << ") ne 1" << endl);
            return -1;
        }

        int iActive     = TC_Common::strto<int>(res[0]["active"]);
        int iInactive   = TC_Common::strto<int>(res[0]["inactive"]);

        if (iActive < 0 || iInactive < 0)
        {
            TLOG_DEBUG("loaded inactive rate failed, active(" << iActive << ") inactive(" << iInactive << ")" << endl);
            return -2;
        }

        return (iInactive + iActive) <= 0 ? 0 : static_cast<int>(100 * iInactive / (iInactive + iActive));
    }
    catch (TC_Mysql_Exception& ex)
    {
        sendSqlErrorAlarmSMS(string("CDbHandle::computeInactiveRate:") + ex.what());
        TLOG_ERROR("CDbHandle::computeInactiveRate exception: " << ex.what() << endl);
        return -3;
    }
    catch (exception& ex)
    {
	    sendSqlErrorAlarmSMS(string("CDbHandle::computeInactiveRate:") + ex.what());
        TLOG_ERROR("CDbHandle::computeInactiveRate " << ex.what() << endl);
        return -4;
    }
}

TC_Mysql::MysqlData CDbHandle::UnionRecord(TC_Mysql::MysqlData& data1, TC_Mysql::MysqlData& data2)
{
    TC_Mysql::MysqlData result;

    result.data() = data1.data();

    vector<map<string, string> >& vTmp = data2.data();
    vector<map<string, string> >::iterator it = vTmp.begin();
    for (; it != vTmp.end(); it++)
    {
        result.data().push_back(*it);
    }

    return result;
}

int CDbHandle::loadObjectIdCache(const bool bRecoverProtect, const int iRecoverProtectRate, const int iLoadTimeInterval, const bool bLoadAll, bool fromInit)
{
    ObjectsCache objectsCache;
    SetDivisionCache setDivisionCache;
    std::map<ServantStatusKey, int> mapStatus;
    std::map<ServantStatusKey, int> mapFlowStatus;

    try
    {
        int64_t iStart = TNOWMS;
        if (bLoadAll)
        {
            //加载分组信息一定要在加载服务信息之前
            loadIPPhysicalGroupInfo(fromInit);
            //加载城市信息
            loadGroupPriority(fromInit);
        }

        //加载存活server及registry列表信息
        string sSql1 =
              "select adapter.servant,adapter.endpoint,server.enable_group,server.setting_state,server.present_state, server.flow_state, server.application,server.server_name,server.node_name,server.enable_set,server.set_name,server.set_area,server.set_group,server.ip_group_name,server.bak_flag "
              "from t_adapter_conf as adapter right join t_server_conf as server using (application, server_name, node_name)";

        //增量加载逻辑
        if (!bLoadAll)
        {
            string sInterval = TC_Common::tm2str(TC_TimeProvider::getInstance()->getNow() - iLoadTimeInterval);
            sSql1 += " ,(select distinct application,server_name from t_server_conf";
            sSql1 += " where registry_timestamp >='" + sInterval + "'";
            sSql1 += " union select distinct application,server_name from t_adapter_conf";
            sSql1 += " where registry_timestamp >='" + sInterval + "') as tmp";
            sSql1 += " where server.application=tmp.application and server.server_name=tmp.server_name";
        }

        string sSql2 = "select servant, endpoint, enable_group, present_state as setting_state, present_state, 'active' as flow_state, tars_version as application, tars_version as server_name, tars_version as node_name,'N' as enable_set,'' as set_name,'' as set_area,'' as set_group ,'' "
                       "as ip_group_name , '' as bak_flag from t_registry_info order by endpoint";

        TC_Mysql::MysqlData res;

        {
            TC_Mysql::MysqlData res1  = _mysqlReg.queryRecord(sSql1);

            TC_Mysql::MysqlData res2  = _mysqlReg.queryRecord(sSql2);
            TLOG_DEBUG("CDbHandle::loadObjectIdCache load " << (bLoadAll ? "all " : "") << "Active objects from db, records affected:" << (res1.size() + res2.size())
                      << "|cost:" << (TNOWMS - iStart) << endl);
            iStart = TNOWMS;
            res = UnionRecord(res1, res2);
        }
        for (unsigned i = 0; i < res.size(); i++)
        {
            try
            {
                if (res[i]["servant"].empty() && res[i]["endpoint"].empty())
                {
                    TLOG_DEBUG(res[i]["application"] << "-" << res[i]["server_name"] << "-" << res[i]["node_name"] << " NULL" << endl);
                    ServantStatusKey statusKey = { res[i]["application"], res[i]["server_name"], res[i]["node_name"] };
                    mapStatus[statusKey] = (res[i]["setting_state"] == "active" && res[i]["present_state"] == "active") ? Active : Inactive;
                    continue;
                }

                TC_Endpoint ep;
                try
                {
                    ep.parse(res[i]["endpoint"]);
                }
                catch (exception& ex)
                {
                    TLOG_ERROR("CDbHandle::loadObjectIdCache " << ex.what() << endl);
                    continue;
                }

                EndpointF epf;
                epf.host        = ep.getHost();
                epf.port        = ep.getPort();
                epf.timeout     = ep.getTimeout();
                epf.weightType  = ep.getWeightType();
                epf.weight      = ep.getWeight();

                // 现在支持三种类型：0 UDP, 1 TCP, 2 SSL
                // 所以istcp字段作为int类型使用
                if (!ep.isTcp()) 
                { 
                    epf.istcp = TC_Endpoint::UDP; 
                } 
                else 
                { 
                    if (ep.isSSL()) 
                        epf.istcp = TC_Endpoint::SSL; 
                    else 
                        epf.istcp = TC_Endpoint::TCP; 
                }

                epf.authType    = ep.getAuthType();
                epf.grouprealid = getGroupId(epf.host);
                string ip_group_name = TC_Common::trim(res[i]["ip_group_name"]);
                epf.grouprealid = ip_group_name.empty() ? getGroupId(epf.host) : getGroupIdByName(ip_group_name);
                epf.groupworkid = TC_Common::lower(res[i]["enable_group"]) == "y" ? epf.grouprealid : -1;
                if (TC_Common::lower(res[i]["enable_group"]) == "y" && epf.grouprealid == -1)
                {
                    //记录查不到分组的组名和ip
                    FDLOG("group_id") << ip_group_name << "|" << epf.host << endl;
                }

                epf.setId       = "";
                epf.bakFlag     = TC_Common::strto<int>(res[i]["bak_flag"]);
                if (epf.bakFlag == 1)
                {
                    // 设置为备机时（bakFlag = 1）时， 节点不返回给客户端 
                    TLOG_DEBUG(res[i]["application"] << "." << res[i]["server_name"] << "-" << res[i]["node_name"] << " bakFlag==1" << endl);
                    continue;
                }

                bool bSet = TC_Common::lower(res[i]["enable_set"]) == "y";
                if (bSet)
                {
                    epf.setId = res[i]["set_name"] + "." + res[i]["set_area"] + "." + res[i]["set_group"];
                }

                //获取权重信息
                epf.weight = ep.getWeight();
                epf.weightType = ep.getWeightType();

                TLOG_DEBUG("CDbHandle::loadObjectIdCache :" << res[i]["servant"] << "." << epf.host << "|"
                                                           << epf.grouprealid << "|" << epf.groupworkid << "|"
                                                           << "weightType: " << epf.weightType << "|" << epf.weight << "|"
                                                           << res[i]["setting_state"] << "|" << res[i]["present_state"]
                                                           << endl);

                bool bActive = true;

                ServantStatusKey statusKey = { res[i]["application"], res[i]["server_name"], res[i]["node_name"] }; 
                
                // if ((res[i]["setting_state"] == "active" && res[i]["present_state"] == "active") 
                //     || res[i]["servant"] == "taf.tafAdminRegistry.AdminRegObj") //如果是管理服务, 强制认为它是活的
                if (res[i]["setting_state"] == "active" && res[i]["present_state"] == "active" && res[i]["flow_state"] != "inactive") 
                {
                    //存活列表
                    objectsCache[res[i]["servant"]].vActiveEndpoints.push_back(epf);
                    mapStatus[statusKey] = Active;
                }
                else
                {
                    //非存活列表
                    objectsCache[res[i]["servant"]].vInactiveEndpoints.push_back(epf);
                    mapStatus[statusKey] = Inactive;
                    bActive = false;
                }

                if (res[i]["flow_state"] == "inactive")
                {
                    mapFlowStatus[statusKey] = Inactive;
                }
                else
                {
                    mapFlowStatus[statusKey] = Active;
                }

                if (bSet)
                {
                    if (res[i]["set_name"].empty() || res[i]["set_area"].empty() || res[i]["set_group"].empty() || res[i]["set_name"] == "*" || res[i]["set_area"] == "*")
                    {
                        TLOG_ERROR("CDbHandle::loadObjectIdCache: " << res[i]["servant"] << "." << epf.host << "|set division invalid[" << res[i]["set_name"] << "." << res[i]["set_area"] << "." << res[i]["set_group"] << "]" << endl);
                        bSet = false;
                    }
                }

                //set划分信息
                if (bSet)
                {
                    //set区域
                    string sSetArea = res[i]["set_name"] + "." + res[i]["set_area"];
                    //set全称
                    string sSetId   = res[i]["set_name"] + "." + res[i]["set_area"] + "." + res[i]["set_group"];

                    SetServerInfo setServerInfo;
                    setServerInfo.bActive = bActive;
                    setServerInfo.epf    = epf;

                    setServerInfo.sSetId = sSetId;
                    setServerInfo.sSetArea = sSetArea;

                    setDivisionCache[res[i]["servant"]][res[i]["set_name"]].push_back(setServerInfo);
                    TLOGINFO("CDbHandle::loadObjectIdCache " << res[i]["servant"] << "." << epf.host << "|" << sSetId << "|" << setServerInfo.bActive << endl);
                }
                else if (!bLoadAll)
                {
                    //增量加载,如果不启用set也要赋个空值，防止更新缓存时不彻底
                    map<string, vector<CDbHandle::SetServerInfo> > mTemp = setDivisionCache[res[i]["servant"]];
                    setDivisionCache[res[i]["servant"]] = mTemp;
                }
            }
            catch (TC_EndpointParse_Exception& ex)
            {
                TLOG_ERROR("CDbHandle::loadObjectIdCache " << ex.what() << endl);
            }
        }

        //替换到cache
        int iRate = bRecoverProtect == true ? computeInactiveRate() : 0;
        if (bRecoverProtect == true && iRate > iRecoverProtectRate && objectsCache.size() > 0)
        {
            TLOG_DEBUG("CDbHandle::loadObjectIdCache  now database recover protect valid, rate:" << iRate << ",iRecoverProtectRate:" << iRecoverProtectRate
                    << std::boolalpha << ",bRecoverProtect:" << bRecoverProtect << endl);
            return -1;
        }

        updateObjectsCache(objectsCache, bLoadAll);
        updateStatusCache(mapStatus, bLoadAll);
        updateFlowStatusCache(mapFlowStatus, bLoadAll);
        updateDivisionCache(setDivisionCache, bLoadAll);

        TLOG_DEBUG("loaded objects to cache  size:" << objectsCache.size() << endl);
        TLOG_DEBUG("loaded server status to cache size:" << mapStatus.size() << endl);
        TLOG_DEBUG("loaded server flow status to cache size:" << mapFlowStatus.size() << endl);
        TLOG_DEBUG("loaded set server to cache size:" << setDivisionCache.size() << endl);
        // FDLOG() << "loaded objects to cache size:" << objectsCache.size() << endl;
        // FDLOG() << "loaded set server to cache size:" << setDivisionCache.size() << endl;

        TLOG_DEBUG("CDbHandle::loadObjectIdCache parse " << (bLoadAll ? "all " : "") << "|cost:" << (TNOWMS - iStart) << endl);
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::loadObjectIdCache exception: " << ex.what() << endl);

        sendSqlErrorAlarmSMS(string("CDbHandle::loadObjectIdCache:") + ex.what());
        if (fromInit)
        {
            //初始化是出现异常，退出
            assert(0);
        }
        return -1;
    }
    catch (exception& ex)
    {
	    sendSqlErrorAlarmSMS(string("CDbHandle::loadObjectIdCache:") + ex.what());
        TLOG_DEBUG("CDbHandle::loadObjectIdCache " << ex.what() << endl);
        if (fromInit)
        {
            //初始化是出现异常，退出
            assert(0);
        }
        return -1;
    }

    return 0;
}

int CDbHandle::updateRegistryInfo2Db(bool bRegHeartbeatOff)
{
    if (bRegHeartbeatOff)
    {
        TLOG_DEBUG("updateRegistryInfo2Db not need to update reigstry status !" << endl);
        return 0;
    }

    map<string, string>::iterator iter;
    map<string, string> mapServantEndpoint = g_app.getServantEndpoint();
    if (mapServantEndpoint.size() == 0)
    {
        TLOG_ERROR("fatal error, get registry servant failed!" << endl);
        return -1;
    }

    try
    {
        string sSql{};

        TC_Endpoint locator;
        locator.parse(mapServantEndpoint[(*g_pconf)["/tars/objname<QueryObjName>"]]);

        for (iter = mapServantEndpoint.begin(); iter != mapServantEndpoint.end(); iter++)
        {
            sSql = "insert into t_registry_info (locator_id, servant, endpoint, last_heartbeat, present_state, tars_version) values ";
            sSql += ("('" + locator.getHost() + ":" + TC_Common::tostr<int>(locator.getPort()) +
                     "','" + iter->first + "', '" + iter->second + "', now(), 'active', " +
                     "'" + _mysqlReg.escapeString(Application::getTarsVersion()) + "')");
            sSql += ("on duplicate key update endpoint='" + iter->second + "',");
            sSql += ("last_heartbeat=now(),present_state='active',");
            sSql += ("tars_version='" + _mysqlReg.escapeString(Application::getTarsVersion()) + "'");
            _mysqlReg.execute(sSql);
        }
    }
    catch (TC_Mysql_Exception& ex)
    {
	    sendSqlErrorAlarmSMS(string("CDbHandle::updateRegistryInfo2Db:") + ex.what());
        TLOG_ERROR("CDbHandle::updateRegistryInfo2Db exception: " << ex.what() << endl);
        return -1;
    }
    catch (exception& ex)
    {
	    sendSqlErrorAlarmSMS(string("CDbHandle::updateRegistryInfo2Db:") + ex.what());
        TLOG_ERROR("CDbHandle::updateRegistryInfo2Db exception: " << ex.what() << endl);
        return -1;
    }

    return 0;
}

vector<EndpointF> CDbHandle::findObjectById(const string& id)
{
    ObjectsCache::iterator it;
    ObjectsCache& usingCache = _objectsCache.getReaderData();

    if ((it = usingCache.find(id)) != usingCache.end())
    {
        // 不能是引用，会改变原始缓存数据
        std::vector<tars::EndpointF> vtEp = it->second.vActiveEndpoints;

        LOAD_BALANCE_INS->getDynamicWeight(id, vtEp);

        return vtEp;
    }
    else
    {
        vector<EndpointF> activeEp;
        return activeEp;
    }
}

int CDbHandle::findObjectById4All(const string& id, vector<EndpointF>& activeEp, vector<EndpointF>& inactiveEp)
{

    TLOG_DEBUG(__FUNCTION__ << " id: " << id << endl);

    ObjectsCache::iterator it;
    ObjectsCache& usingCache = _objectsCache.getReaderData();

    if ((it = usingCache.find(id)) != usingCache.end())
    {
        activeEp   = it->second.vActiveEndpoints;
        inactiveEp = it->second.vInactiveEndpoints;

        LOAD_BALANCE_INS->getDynamicWeight(id, activeEp);
    }
    else
    {
        activeEp.clear();
        inactiveEp.clear();
    }

    return  0;
}

vector<EndpointF> CDbHandle::getEpsByGroupId(const vector<EndpointF>& vecEps, const GroupUseSelect GroupSelect, int iGroupId, ostringstream& os)
{
    os << "|";
    vector<EndpointF> vResult;

    for (unsigned i = 0; i < vecEps.size(); i++)
    {
        os << vecEps[i].host << ":" << vecEps[i].port << "(" << vecEps[i].groupworkid << ");";
        if (GroupSelect == ENUM_USE_WORK_GROUPID && vecEps[i].groupworkid == iGroupId)
        {
            vResult.push_back(vecEps[i]);
        }
        if (GroupSelect == ENUM_USE_REAL_GROUPID && vecEps[i].grouprealid == iGroupId)
        {
            vResult.push_back(vecEps[i]);
        }
    }

    return vResult;
}

vector<EndpointF> CDbHandle::getEpsByGroupId(const vector<EndpointF>& vecEps, const GroupUseSelect GroupSelect, const set<int>& setGroupID, ostringstream& os)
{
    os << "|";
    std::vector<EndpointF> vecResult;

    for (std::vector<EndpointF>::size_type i = 0; i < vecEps.size(); i++)
    {
        os << vecEps[i].host << ":" << vecEps[i].port << "(" << vecEps[i].groupworkid << ")";
        if (GroupSelect == ENUM_USE_WORK_GROUPID && setGroupID.count(vecEps[i].groupworkid) == 1)
        {
            vecResult.push_back(vecEps[i]);
        }
        if (GroupSelect == ENUM_USE_REAL_GROUPID && setGroupID.count(vecEps[i].grouprealid) == 1)
        {
            vecResult.push_back(vecEps[i]);
        }
    }

    return vecResult;
}

int CDbHandle::findObjectByIdInSameGroup(const string& id, const string& ip, vector<EndpointF>& activeEp, vector<EndpointF>& inactiveEp, ostringstream& os)
{
    activeEp.clear();
    inactiveEp.clear();

    int iClientGroupId  = getGroupId(ip);

    os << "|(" << iClientGroupId << ")";

    if (iClientGroupId == -1)
    {
        return findObjectById4All(id, activeEp, inactiveEp);
    }

    ObjectsCache::iterator it;
    ObjectsCache& usingCache = _objectsCache.getReaderData();

    if ((it = usingCache.find(id)) != usingCache.end())
    {
        activeEp    = getEpsByGroupId(it->second.vActiveEndpoints, ENUM_USE_WORK_GROUPID, iClientGroupId, os);
        inactiveEp  = getEpsByGroupId(it->second.vInactiveEndpoints, ENUM_USE_WORK_GROUPID, iClientGroupId, os);

        if (activeEp.size() == 0) //没有同组的endpoit,匹配未启用分组的服务
        {
            activeEp    = getEpsByGroupId(it->second.vActiveEndpoints, ENUM_USE_WORK_GROUPID, -1, os);
            inactiveEp  = getEpsByGroupId(it->second.vInactiveEndpoints, ENUM_USE_WORK_GROUPID, -1, os);
        }
        if (activeEp.size() == 0) //没有同组的endpoit
        {
            activeEp   = it->second.vActiveEndpoints;
            inactiveEp = it->second.vInactiveEndpoints;
        }
    }

    LOAD_BALANCE_INS->getDynamicWeight(id, activeEp);

    return  0;
}

int CDbHandle::findObjectByIdInGroupPriority(const std::string& sID, const std::string& sIP, std::vector<EndpointF>& vecActive, std::vector<EndpointF>& vecInactive, std::ostringstream& os)
{
    vecActive.clear();
    vecInactive.clear();

    int iClientGroupID = getGroupId(sIP);
    os << "|(" << iClientGroupID << ")";
    if (iClientGroupID == -1)
    {
        return findObjectById4All(sID, vecActive, vecInactive);
    }

    ObjectsCache& usingCache = _objectsCache.getReaderData();
    ObjectsCache::iterator itObject = usingCache.find(sID);
    if (itObject == usingCache.end()) return 0;

    //首先在同组中查找
    {
        vecActive     = getEpsByGroupId(itObject->second.vActiveEndpoints, ENUM_USE_WORK_GROUPID, iClientGroupID, os);
        vecInactive    = getEpsByGroupId(itObject->second.vInactiveEndpoints, ENUM_USE_WORK_GROUPID, iClientGroupID, os);
        os << "|(In Same Group: " << iClientGroupID << " Active=" << vecActive.size() << " Inactive=" << vecInactive.size() << ")";
    }

    //启用分组，但同组中没有找到，在优先级序列中查找
    std::map<int, GroupPriorityEntry> & mapPriority = _mapGroupPriority.getReaderData();
    for (std::map<int, GroupPriorityEntry>::iterator it = mapPriority.begin(); it != mapPriority.end() && vecActive.empty(); it++)
    {
        if (it->second.setGroupID.count(iClientGroupID) == 0)
        {
            os << "|(Not In Priority " << it->second.sGroupID << ")";
            continue;
        }
        vecActive    = getEpsByGroupId(itObject->second.vActiveEndpoints, ENUM_USE_WORK_GROUPID, it->second.setGroupID, os);
        vecInactive    = getEpsByGroupId(itObject->second.vInactiveEndpoints, ENUM_USE_WORK_GROUPID, it->second.setGroupID, os);
        os << "|(In Priority: " << it->second.sGroupID << " Active=" << vecActive.size() << " Inactive=" << vecInactive.size() << ")";
    }

    //没有同组的endpoit,匹配未启用分组的服务
    if (vecActive.empty())
    {
        vecActive    = getEpsByGroupId(itObject->second.vActiveEndpoints, ENUM_USE_WORK_GROUPID, -1, os);
        vecInactive    = getEpsByGroupId(itObject->second.vInactiveEndpoints, ENUM_USE_WORK_GROUPID, -1, os);
        os << "|(In No Grouop: Active=" << vecActive.size() << " Inactive=" << vecInactive.size() << ")";
    }

    //在未分组的情况下也没有找到，返回全部地址(此时基本上所有的服务都已挂掉)
    if (vecActive.empty())
    {
        vecActive    = itObject->second.vActiveEndpoints;
        vecInactive    = itObject->second.vInactiveEndpoints;
        os << "|(In All: Active=" << vecActive.size() << " Inactive=" << vecInactive.size() << ")";
    }

    LOAD_BALANCE_INS->getDynamicWeight(sID, vecActive);

    return 0;
}

int CDbHandle::findObjectByIdInSameStation(const std::string& sID, const std::string& sStation, std::vector<EndpointF>& vecActive, std::vector<EndpointF>& vecInactive, std::ostringstream& os)
{
    vecActive.clear();
    vecInactive.clear();

    //获得station所有组
    std::map<int, GroupPriorityEntry> & mapPriority         = _mapGroupPriority.getReaderData();
    std::map<int, GroupPriorityEntry>::iterator itGroup     = mapPriority.end();
    for (itGroup = mapPriority.begin(); itGroup != mapPriority.end(); itGroup++)
    {
        if (itGroup->second.sStation != sStation) continue;

        break;
    }

    if (itGroup == mapPriority.end())
    {
        os << "|not found station:" << sStation;
        return -1;
    }

    ObjectsCache& usingCache = _objectsCache.getReaderData();
    ObjectsCache::iterator itObject = usingCache.find(sID);
    if (itObject == usingCache.end()) return 0;

    //查找对应所有组下的IP地址
    vecActive    = getEpsByGroupId(itObject->second.vActiveEndpoints, ENUM_USE_REAL_GROUPID, itGroup->second.setGroupID, os);
    vecInactive    = getEpsByGroupId(itObject->second.vInactiveEndpoints, ENUM_USE_REAL_GROUPID, itGroup->second.setGroupID, os);

    LOAD_BALANCE_INS->getDynamicWeight(sID, vecActive);

    return 0;
}

int CDbHandle::findObjectByIdInSameSet(const string& sID, const vector<string>& vtSetInfo, std::vector<EndpointF>& vecActive, std::vector<EndpointF>& vecInactive, std::ostringstream& os)
{
    string sSetName   = vtSetInfo[0];
    string sSetArea   = vtSetInfo[0] + "." + vtSetInfo[1];
    string sSetId     = vtSetInfo[0] + "." + vtSetInfo[1] + "." + vtSetInfo[2];

    SetDivisionCache& usingSetDivisionCache = _setDivisionCache.getReaderData();
    SetDivisionCache::iterator it = usingSetDivisionCache.find(sID);
    if (it == usingSetDivisionCache.end())
    {
        //此情况下没启动set
        TLOGINFO("CDbHandle::findObjectByIdInSameSet:" << __LINE__ << "|" << sID << " haven't start set|" << sSetId << endl);
        return -1;
    }

    map<string, vector<SetServerInfo> >::iterator setNameIt = it->second.find(sSetName);
    if (setNameIt == (it->second).end())
    {
        //此情况下没启动set
        TLOGINFO("CDbHandle::findObjectByIdInSameSet:" << __LINE__ << "|" << sID << " haven't start set|" << sSetId << endl);
        return -1;
    }

    if (vtSetInfo[2] == "*")
    {
        //检索通配组和set组中的所有服务
        vector<SetServerInfo>  vServerInfo = setNameIt->second;
        for (size_t i = 0; i < vServerInfo.size(); i++)
        {
            if (vServerInfo[i].sSetArea == sSetArea)
            {
                if (vServerInfo[i].bActive)
                {
                    vecActive.push_back(vServerInfo[i].epf);
                }
                else
                {
                    vecInactive.push_back(vServerInfo[i].epf);
                }
            }
        }

        LOAD_BALANCE_INS->getDynamicWeight(sID, vecActive);

        return (vecActive.empty() && vecInactive.empty()) ? -2 : 0;
    }
    else
    {

        // 1.从指定set组中查找
        int iRet = findObjectByIdInSameSet(sSetId, setNameIt->second, vecActive, vecInactive, os);
        if (iRet != 0 && vtSetInfo[2] != "*")
        {
            // 2. 步骤1中没找到，在通配组里找
            string sWildSetId =  vtSetInfo[0] + "." + vtSetInfo[1] + ".*";
            iRet = findObjectByIdInSameSet(sWildSetId, setNameIt->second, vecActive, vecInactive, os);
        }

        LOAD_BALANCE_INS->getDynamicWeight(sID, vecActive);

        return iRet;
    }


}

int CDbHandle::findObjectByIdInSameSet(const string& sSetId, const vector<SetServerInfo>& vSetServerInfo, std::vector<EndpointF>& vecActive, std::vector<EndpointF>& vecInactive, std::ostringstream& os)
{
    for (size_t i = 0; i < vSetServerInfo.size(); ++i)
    {
        if (vSetServerInfo[i].sSetId == sSetId)
        {
            if (vSetServerInfo[i].bActive)
            {
                vecActive.push_back(vSetServerInfo[i].epf);
            }
            else
            {
                vecInactive.push_back(vSetServerInfo[i].epf);
            }
        }
    }

    int iRet = (vecActive.empty() && vecInactive.empty()) ? -2 : 0;
    return iRet;
}
int CDbHandle::getNodeTemplateName(const string nodeName, string& sTemplateName)
{
    try
    {
        string sSql =
                      "select template_name "
                      "from t_node_info "
                      "where node_name='" + _mysqlReg.escapeString(nodeName) + "'";

        TC_Mysql::MysqlData res = _mysqlReg.queryRecord(sSql);

        if (res.size() != 0)
        {
            sTemplateName = res[0]["template_name"];
        }

        TLOG_DEBUG("CDbHandle::getNodeTemplateName '" << nodeName << "' affected:" << res.size()
                  << " get template_name:'" << sTemplateName << "'" << endl);

        return 0;
    }
    catch (TC_Mysql_Exception& ex)
    {
        TLOG_ERROR("CDbHandle::getNodeTemplateName exception: " << ex.what() << endl);
        return -1;
    }

    return 0;
}

void CDbHandle::updateStatusCache(const std::map<ServantStatusKey, int>& mStatus, bool updateAll)
{
    TC_ThreadLock::Lock lock(_mapServantStatusLock);
    if (updateAll)
    {
        //全量更新
        _mapServantStatus = mStatus;
    }
    else
    {
        std::map<ServantStatusKey, int>::const_iterator it = mStatus.begin();
        for (; it != mStatus.end(); it++)
        {
            _mapServantStatus[it->first] = it->second;
        }
    }
}

void CDbHandle::updateFlowStatusCache(const std::map<ServantStatusKey, int>& mStatus, bool updateAll)
{
    TC_ThreadLock::Lock lock(_mapServantFlowStatusLock);
    if (updateAll)
    {
        //全量更新
        _mapServantFlowStatus = mStatus;
    }
    else
    {
        std::map<ServantStatusKey, int>::const_iterator it = mStatus.begin();
        for (; it != mStatus.end(); it++)
        {
            _mapServantFlowStatus[it->first] = it->second;
        }
    }
}

void CDbHandle::updateObjectsCache(const ObjectsCache& objCache, bool updateAll)
{
    //全量更新
    if (updateAll)
    {
        _objectsCache.getWriterData() = objCache;
        _objectsCache.swap();
    }
    else
    {
        //用查询数据覆盖一下
        _objectsCache.getWriterData() = _objectsCache.getReaderData();
        ObjectsCache& tmpObjCache = _objectsCache.getWriterData();

        ObjectsCache::const_iterator it = objCache.begin();
        for (; it != objCache.end(); it++)
        {
            //增量的时候加载的是服务的所有节点，因此这里直接替换
            tmpObjCache[it->first] = it->second;
        }
        _objectsCache.swap();
    }
}

void CDbHandle::updateDivisionCache(const SetDivisionCache& setDivisionCache, bool updateAll)
{
    //全量更新
    if (updateAll)
    {
        _setDivisionCache.getWriterData() = setDivisionCache;
        _setDivisionCache.swap();
    }
    else
    {
        _setDivisionCache.getWriterData() = _setDivisionCache.getReaderData();
        SetDivisionCache& tmpsetCache = _setDivisionCache.getWriterData();
        SetDivisionCache::const_iterator it = setDivisionCache.begin();
        for (; it != setDivisionCache.end(); it++)
        {
            //有set信息才更新
            if (it->second.size() > 0)
            {
                tmpsetCache[it->first] = it->second;
            }
            else if (tmpsetCache.count(it->first))
            {
                //这个服务的所有节点都没有启用set，删除缓存中的set信息
                tmpsetCache.erase(it->first);
            }
        }
        _setDivisionCache.swap();
    }
}
void CDbHandle::sendSqlErrorAlarmSMS(const string &err)
{
    string errInfo = " ERROR:" + g_app.getAdapterEndpoint().getHost() +  ": registry error: " + err + ", please check!";
    TARS_NOTIFY_ERROR(errInfo);

    TLOG_ERROR("TARS_NOTIFY_ERROR " << errInfo << endl);
}

uint32_t CDbHandle::stringIpToInt(const std::string& sip)
{
    string ip1, ip2, ip3, ip4;
    uint32_t dip, p1, p2, p3;
    dip = 0;
    p1 = sip.find('.');
    p2 = sip.find('.', p1 + 1);
    p3 = sip.find('.', p2 + 1);
    ip1 = sip.substr(0, p1);
    ip2 = sip.substr(p1 + 1, p2 - p1 - 1);
    ip3 = sip.substr(p2 + 1, p3 - p2 - 1);
    ip4 = sip.substr(p3 + 1, sip.size() - p3 - 1);
    (((unsigned char *)&dip)[0]) = TC_Common::strto<unsigned int>(ip1);
    (((unsigned char *)&dip)[1]) = TC_Common::strto<unsigned int>(ip2);
    (((unsigned char *)&dip)[2]) = TC_Common::strto<unsigned int>(ip3);
    (((unsigned char *)&dip)[3]) = TC_Common::strto<unsigned int>(ip4);
    return htonl(dip);
}

string CDbHandle::Ip2Str(uint32_t ip)
{
    char str[50];
    unsigned char  *p = (unsigned char *)&ip;
    sprintf(str, "%u.%u.%u.%u", p[3], p[2], p[1], p[0]);
    return string(str);
}

string CDbHandle::Ip2StarStr(uint32_t ip)
{
    char str[50];
    unsigned char  *p = (unsigned char *)&ip;
    sprintf(str, "%u.%u.%u.*", p[3], p[2], p[1]);
    return string(str);
}

int CDbHandle::updateServerFlowState(const string & app, const string & serverName, const vector<string>& nodeList, bool bActive)
{
    TLOG_DEBUG("CDbHandle::updateServerFlowState:" << app << "." << serverName << ", " << TC_Common::tostr(nodeList) << ", status:" << (bActive ? "active" : "inactive") << endl);
    TC_ThreadLock::Lock lock(_mapServantFlowStatusLock);
    for (size_t i = 0; i < nodeList.size(); i++)
    {
        ServantStatusKey statusKey = {app, serverName, nodeList[i]};
        if (bActive) 
        {
            _mapServantFlowStatus[statusKey] = Active;
        }
        else
        {
            _mapServantFlowStatus[statusKey] = Inactive;
        }
    }

    return 0;
}

int CDbHandle::getFrameworkKey(FrameworkKey &fKey)
{
	auto data = _mysqlReg.queryRecord("select * from t_framework_key");

	if(data.size() > 0)
	{
		fKey.cuid = data[0]["cuid"];
		fKey.priKey = data[0]["pri_key"];
	}

	return 0;
}

/*****************************************************************************
 * Copyright (C) 2012
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#define __STDC_CONSTANT_MACROS 1

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_network.h>
#include <vlc_services_discovery.h>

#include <unordered_map>
#include <sstream>

#include "discovery.h"
#include "helper.h"
#include "htsmessage.h"
#include "sha1.h"

struct tmp_channel
{
	std::string name;
	uint32_t cid;
	uint32_t cnum;
	std::string url;
	input_item_t *item;
};

struct services_discovery_sys_t : public sys_common_t
{
	services_discovery_sys_t()
	{}

	vlc_thread_t thread;
	std::unordered_map<uint32_t, tmp_channel> channelMap;
};

bool ConnectSD(services_discovery_t *sd)
{
	services_discovery_sys_t *sys = sd->p_sys;

	const char *host = var_GetString(sd, CFG_PREFIX"host");
	int port = var_GetInteger(sd, CFG_PREFIX"port");

	if(host == 0 || host[0] == 0)
		host = "localhost";

	if(port == 0)
		port = 9982;

	sys->netfd = net_ConnectTCP(sd, host, port);

	if(sys->netfd < 0)
	{
		msg_Err(sd, "net_ConnectTCP failed");
		return false;
	}

	HtsMap map;
	map.setData("method", "hello");
	map.setData("clientname", "VLC media player");
	map.setData("htspversion", 8);

	HtsMessage m = ReadResult(sd, sys, map.makeMsg());
	if(!m.isValid())
	{
		msg_Err(sd, "No valid hello response");
		return false;
	}

	uint32_t chall_len;
	void * chall;
	m.getRoot()->getBin("challenge", &chall_len, &chall);

	std::string serverName = m.getRoot()->getStr("servername");
	std::string serverVersion = m.getRoot()->getStr("serverversion");
	uint32_t protoVersion = m.getRoot()->getU32("htspversion");

	msg_Info(sd, "Connected to HTSP Server %s, version %s, protocol %d", serverName.c_str(), serverVersion.c_str(), protoVersion);

	const char *user = var_GetString(sd, CFG_PREFIX"user");
	const char *pass = var_GetString(sd, CFG_PREFIX"pass");
	if(user == 0 || user[0] == 0)
		return true;

	map = HtsMap();
	map.setData("method", "authenticate");
	map.setData("username", user);

	if(pass != 0 && pass[0] != 0 && chall)
	{
		msg_Info(sd, "Authenticating as '%s' with a password", user);

		HTSSHA1 *shactx = (HTSSHA1*)malloc(hts_sha1_size);
		uint8_t d[20];
		hts_sha1_init(shactx);
		hts_sha1_update(shactx, (const uint8_t *)pass, strlen(pass));
		hts_sha1_update(shactx, (const uint8_t *)chall, chall_len);
		hts_sha1_final(shactx, d);

		std::shared_ptr<HtsBin> bin = std::make_shared<HtsBin>();
		bin->setBin(20, d);
		map.setData("digest", bin);

		free(shactx);
	}
	else
		msg_Info(sd, "Authenticating as '%s' without a password", user);

	if(chall)
		free(chall);

	bool res = ReadSuccess(sd, sys, map.makeMsg(), "authenticate");
	if(res)
		msg_Info(sd, "Successfully authenticated!");
	else
		msg_Err(sd, "Authentication failed!");
	return res;
}

bool compare_tmp_channel(tmp_channel first, tmp_channel second)
{
	if(first.cnum < second.cnum)
		return true;
	return false;
}

bool GetChannels(services_discovery_t *sd)
{
	services_discovery_sys_t *sys = sd->p_sys;

	HtsMap map;
	map.setData("method", "enableAsyncMetadata");
	if(!ReadSuccess(sd, sys, map.makeMsg(), "enable async metadata"))
		return false;

	std::list<tmp_channel> channels;

	HtsMessage m;
	while((m = ReadMessage(sd, sys)).isValid())
	{
		std::string method = m.getRoot()->getStr("method");
		if(method.empty() || method == "initialSyncCompleted")
		{
			msg_Info(sd, "Finished getting initial metadata sync");
			break;
		}

		if(method == "channelAdd")
		{
			if(!m.getRoot()->contains("channelId"))
				continue;
			uint32_t cid = m.getRoot()->getU32("channelId");

			std::string cname = m.getRoot()->getStr("channelName");
			if(cname.empty())
				continue;

			if(!m.getRoot()->contains("channelNumber"))
				continue;
			uint32_t cnum = m.getRoot()->getU32("channelNumber");

			std::ostringstream oss;
			oss << "htsp://";

			char *user = var_GetString(sd, CFG_PREFIX"user");
			char *pass = var_GetString(sd, CFG_PREFIX"pass");
			if(user != 0 && user[0] != 0 && pass != 0 && pass[0] != 0)
				oss << user << ":" << pass << "@";
			else if(user != 0 && user[0] != 0)
				oss << user << "@";

			const char *host = var_GetString(sd, CFG_PREFIX"host");
			if(host == 0 || host[0] == 0)
				host = "localhost";
			int port = var_GetInteger(sd, CFG_PREFIX"port");
			if(port == 0)
				port = 9982;
			oss << host << ":" << port << "/" << cid;

			tmp_channel ch;
			ch.name = cname;
			ch.cid = cid;
			ch.cnum = cnum;
			ch.url = oss.str();
			channels.push_back(ch);
		}
	}

	channels.sort(compare_tmp_channel);

	while(channels.size() > 0)
	{
		tmp_channel ch = channels.front();
		channels.pop_front();

		ch.item = input_item_New(ch.url.c_str(), ch.name.c_str());
		if(unlikely(ch.item == 0))
			return false;

		services_discovery_AddItem(sd, ch.item, "Channels");

		sys->channelMap[ch.cid] = ch;
	}

	return true;
}

void * RunSD(void *obj)
{
	services_discovery_t *sd = (services_discovery_t *)obj;
	services_discovery_sys_t *sys = sd->p_sys;

	if(!ConnectSD(sd))
	{
		msg_Err(sd, "Connecting to HTS Failed!");
		return 0;
	}

	GetChannels(sd);

	for(;;)
	{
		HtsMessage msg = ReadMessage(sd, sys);
		if(!msg.isValid())
			return 0;

		std::string method = msg.getRoot()->getStr("method");
		if(method.empty())
			return 0;

		msg_Dbg(sd, "Got Message with method %s", method.c_str());
	}

	return 0;
}

int OpenSD(vlc_object_t *obj)
{
	services_discovery_t *sd = (services_discovery_t *)obj;
	services_discovery_sys_t *sys = new services_discovery_sys_t;
	if(unlikely(sys == NULL))
		return VLC_ENOMEM;
	sd->p_sys = sys;

	config_ChainParse(sd, CFG_PREFIX, cfg_options, sd->p_cfg);

	if(vlc_clone(&sys->thread, RunSD, sd, VLC_THREAD_PRIORITY_LOW))
	{
		delete sys;
		return VLC_EGENERIC;
	}

	return VLC_SUCCESS;
}

void CloseSD(vlc_object_t *obj)
{
	services_discovery_t *sd = (services_discovery_t *)obj;
	services_discovery_sys_t *sys = sd->p_sys;

	if(!sys)
		return;

	vlc_cancel(sys->thread);
	vlc_join(sys->thread, 0);

	delete sys;
	sys = sd->p_sys = 0;
}

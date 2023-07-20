/*
 * WebSocketServer.cpp
 *
 * Copyright (C) 2023 Max Qian <lightapt.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/*************************************************

Copyright: 2023 Max Qian. All rights reserved

Author: Max Qian

E-mail: astro_air@126.com

Date: 2023-7-13

Description: WebSocket Server

**************************************************/

#include "WebSocketServer.hpp"

#include <functional>
#include <iostream>
#include <version>
#include <thread>

#include "LithiumApp.hpp"

WebSocketServer::WebSocketServer()
{
	m_CommandDispatcher = std::make_unique<CommandDispatcher>();

	LiRegisterFunc("RunDeviceTask", &WebSocketServer::RunDeviceTask);
	LiRegisterFunc("GetDeviceInfo", &WebSocketServer::GetDeviceInfo);
	LiRegisterFunc("GetDeviceList", &WebSocketServer::GetDeviceList);
	LiRegisterFunc("AddDevice",&WebSocketServer::AddDevice);
	LiRegisterFunc("AddDeviceLibrary",&WebSocketServer::AddDeviceLibrary);
	LiRegisterFunc("RemoveDevice",&WebSocketServer::RemoveDevice);
	LiRegisterFunc("RemoveDeviceByName",&WebSocketServer::RemoveDevicesByName);
	LiRegisterFunc("RemoveDeviceLibrary",&WebSocketServer::RemoveDeviceLibrary);
}

void WebSocketServer::onPing(const WebSocket &socket, const oatpp::String &message)
{
	OATPP_LOGD(TAG, "onPing");
	socket.sendPong(message);
}

void WebSocketServer::onPong(const WebSocket &socket, const oatpp::String &message)
{
	OATPP_LOGD(TAG, "onPong");
}

void WebSocketServer::onClose(const WebSocket &socket, v_uint16 code, const oatpp::String &message)
{
	OATPP_LOGD(TAG, "onClose code=%d", code);
}

void WebSocketServer::readMessage(const WebSocket &socket, v_uint8 opcode, p_char8 data, oatpp::v_io_size size)
{
	if (size == 0)
	{
		auto wholeMessage = m_messageBuffer.toString();
		m_messageBuffer.setCurrentPosition(0);
		OATPP_LOGD(TAG, "onMessage message='%s'", wholeMessage->c_str());
		if (!nlohmann::json::accept(wholeMessage->c_str()))
		{
			OATPP_LOGE("WSServer", "Message is not in JSON format");
			return;
		}
		try
		{
			OATPP_LOGD("WSServer", "Start client command in alone thread");
			nlohmann::json jdata = nlohmann::json::parse(wholeMessage->c_str());
#if __cplusplus >= 202002L
			std::jthread myThread(std::bind(&WebSocketServer::ProcessMessage, this, std::ref(socket), std::ref(jdata)));
#else
			std::thread myThread(std::bind(&WebSocketServer::ProcessMessage, this, std::ref(socket), std::ref(jdata)));
#endif
			myThread.detach();
			OATPP_LOGD("WSServer", "Started command thread successfully");
		}
		catch (const nlohmann::detail::parse_error &e)
		{
			OATPP_LOGE("WSServer", "Failed to parser JSON message : %s", e.what());
		}
		catch (const std::exception &e)
		{
			OATPP_LOGE("WSServer", "Unknown error happened in WebsocketServer : %s", e.what());
		}
	}
	else if (size > 0)
	{ // message frame received
		m_messageBuffer.writeSimple(data, size);
	}
}

void WebSocketServer::ProcessMessage(const WebSocket &socket, const nlohmann::json &data)
{
	try
	{
		if (data.empty())
		{
			OATPP_LOGE("WSServer", "WebSocketServer::processMessage() data is empty");
			return;
		}
		nlohmann::json reply_data;
		try
		{
			if (data.contains("name") && data.contains("params"))
			{
				const std::string name = data["name"].get<std::string>();
				if (m_CommandDispatcher->HasHandler(name))
				{
					json res = m_CommandDispatcher->Dispatch(name, data["params"].get<json>());
					if (res.contains("error"))
					{
						OATPP_LOGE("WSServer", "Failed to run command %s , error : %s", name.c_str(), res.dump().c_str());
						reply_data["error"] = res["error"];
					}
					else
					{
						OATPP_LOGD("WSServer", "Run command %s successfully", name.c_str());
						reply_data = {{"reply", "OK"}};
					}
				}
			}
			else
			{
				OATPP_LOGE("WSServer", "WebSocketServer::processMessage() missing parameter: name or params");
				reply_data = {{"error", "Missing parameter: name or params"}};
			}
		}
		catch (const nlohmann::json::exception &e)
		{
			OATPP_LOGE("WSServer", "WebSocketServer::processMessage() json exception: %s", e.what());
			reply_data = {{"error", e.what()}};
		}
		catch (const std::exception &e)
		{
			OATPP_LOGE("WSServer", "WebSocketServer::processMessage() exception: %s", e.what());
			reply_data = {{"error", e.what()}};
		}
		socket.sendOneFrameText(reply_data.dump());
	}
	catch (const std::exception &e)
	{
		OATPP_LOGE("WSServer", "WebSocketServer::onMessage() parse json failed: %s", e.what());
	}
}

std::atomic<v_int32> WSInstanceListener::SOCKETS(0);

void WSInstanceListener::onAfterCreate(const oatpp::websocket::WebSocket &socket, const std::shared_ptr<const ParameterMap> &params)
{

	SOCKETS++;
	OATPP_LOGD(TAG, "New Incoming Connection. Connection count=%d", SOCKETS.load());

	/* In this particular case we create one WebSocketServer per each connection */
	/* Which may be redundant in many cases */
	socket.setListener(std::make_shared<WebSocketServer>());
}

void WSInstanceListener::onBeforeDestroy(const oatpp::websocket::WebSocket &socket)
{

	SOCKETS--;
	OATPP_LOGD(TAG, "Connection closed. Connection count=%d", SOCKETS.load());
}
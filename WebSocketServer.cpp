//
// WebSocketServer.cpp
//
// This sample demonstrates the WebSocket class.
//
// Copyright (c) 2012, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//
 
#include <stdio.h>

#ifndef _WIN32
#include <sys/time.h>
#include <unistd.h>
#endif

#include "Poco/Net/HTTPServer.h"
#include "Poco/Net/HTTPRequestHandler.h"
#include "Poco/Net/HTTPRequestHandlerFactory.h"
#include "Poco/Net/HTTPServerParams.h"
#include "Poco/Net/HTTPServerRequest.h"
#include "Poco/Net/HTTPServerResponse.h"
#include "Poco/Net/HTTPServerParams.h"
#include "Poco/Net/ServerSocket.h"
#include "Poco/Net/WebSocket.h"
#include "Poco/Net/NetException.h"
#include "Poco/Util/ServerApplication.h"
#include "Poco/Util/Option.h"
#include "Poco/Util/OptionSet.h"
#include "Poco/Util/HelpFormatter.h"
#include "Poco/Format.h"
#include <iostream>
#include <iomanip>
#include <mutex>
#include <map>

#include "rapidjson/writer.h"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"

#define MAX_APPS			250
#define APP_KEY_LEN			64

#define ACT_ADD_HOST		1
#define ACT_RES_HOST		2
#define ACT_RES_CLIENT		3

#define STRINGIFY(a, b) 	StringBuffer b; Writer<StringBuffer> writer(b); a.Accept(writer);

#ifndef _WIN32
typedef unsigned long DWORD;
#endif

typedef struct {
	int offset;
	int length;
} COMPRESSED_INDEX;

using namespace rapidjson;

using Poco::Net::ServerSocket;
using Poco::Net::WebSocket;
using Poco::Net::WebSocketException;
using Poco::Net::HTTPRequestHandler;
using Poco::Net::HTTPRequestHandlerFactory;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServerResponse;
using Poco::Net::HTTPServerParams;
using Poco::Timestamp;
using Poco::ThreadPool;
using Poco::Util::ServerApplication;
using Poco::Util::Application;
using Poco::Util::Option;
using Poco::Util::OptionSet;
using Poco::Util::HelpFormatter;

#ifndef _WIN32

DWORD GetTickCount()
{
    struct timeval gettick;
    
    gettimeofday(&gettick, NULL);
    
    return gettick.tv_sec*1000 + gettick.tv_usec/1000;
}

#endif

std::string asHex(DWORD num) {
	const std::string table = "0123456789abcdef"; // lookup hexadecimal
	DWORD x = num; // make negative complement
	std::string r = "";
	while (x > 0) {
		int y = x % 16; // get remainder
		r = table[y] + r; // concatenate in the reverse order
		x /= 16; 
	}
	return r == "" ? "0" : r;
}

std::string asDec(DWORD num) {
	const std::string table = "0123456789"; // lookup hexadecimal
	DWORD x = num; // make negative complement
	std::string r = "";
	while (x > 0) {
		int y = x % 10; // get remainder
		r = table[y] + r; // concatenate in the reverse order
		x /= 10; 
	}
	return r == "" ? "0" : r;
}

void set_string(Value & jObj, const char * sKey, std::string sValue, Document::AllocatorType & a)
{
	Value o, k;
	
	o.SetString(sValue.data(), sValue.size(), a);
	k.SetString(sKey, strlen(sKey), a);

	jObj.AddMember(k, o, a);
}

void set_value(Value & jObj, std::string sKey, Value & jSrc, Document::AllocatorType & a)
{
	Value k;

	k.SetString(sKey.data(), sKey.size(), a);

	jObj.AddMember(k, jSrc, a);
}

std::string gen_random(const int len) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

	std::string s = asHex(GetTickCount());
	
    for (int i = s.size(); i < len; ++i) {
        s += alphanum[rand() % (sizeof(alphanum) - 1)];
    }
	
	return s;
}

void JsonDecode(const Value & jData, std::vector<COMPRESSED_INDEX> & indexes)
{
	COMPRESSED_INDEX ci;

	for (Value::ConstValueIterator iter = jData.Begin(); iter != jData.End(); iter++) {
		ci.offset = (*iter)["offset"].GetInt();
		ci.length = (*iter)["length"].GetInt();
		indexes.push_back(ci);
	}
}

bool FindIndex(const std::vector<COMPRESSED_INDEX> & indexes, int idx)
{
	int n, cnt;
	for(std::vector<COMPRESSED_INDEX>::const_iterator iter = indexes.begin(); iter != indexes.end(); iter++) {
		n = (*iter).offset;
		cnt = (*iter).offset + (*iter).length;
		while(n<cnt) {
			if (n == idx) return true;
			++n;
		}
	}
	return false;	
}

class WebSocketRequestHandler: public HTTPRequestHandler
	/// Handle a WebSocket connection.
{
public:
	void handleRequest(HTTPServerRequest& request, HTTPServerResponse& response)
	{
		Application& app = Application::instance();
		try
		{
			WebSocket ws(request, response);
			app.logger().information("WebSocket connection established.");
			char * buffer = NULL;
			int flags;
			int n = 0, m;

			do
			{
				Document jRes;
				Value jArr(kArrayType);
				Document::AllocatorType & a = jRes.GetAllocator();

				jRes.SetObject();

				m = ws.getReceiveBufferSize();
				if (m > 0) {
					Document jDoc;

					if (buffer) delete buffer;
 
					buffer = new char[m + 1]; 

					n = ws.receiveFrame(buffer, m, flags);
					if (n > 0) {
						buffer[n] = 0;

						if (!jDoc.Parse<0>(buffer).HasParseError() && jDoc.IsObject()) {
							m_mutex.lock();
							if (jDoc.HasMember("app_id")) {
								int app_id = jDoc["app_id"].GetInt();
								int act = jDoc["action"].GetInt();
								if (app_id >= 0 && app_id < MAX_APPS) {
									std::vector<COMPRESSED_INDEX> indexes;

									jRes.AddMember("app_id", app_id, a);
									jRes.AddMember("action", act, a);

									switch(act) {
									case ACT_ADD_HOST:
										if (m_stARCSApps[app_id].empty()) {
											m_stARCSApps[app_id] = gen_random(APP_KEY_LEN);
										}
										set_string(jRes, "key", m_stARCSApps[app_id], a);
										break;
									case ACT_RES_HOST:
										if (jDoc.HasMember("key")) {
											if (m_stARCSApps[app_id] ==  jDoc["key"].GetString() && jDoc.HasMember("data")) {

												for (Value::ConstMemberIterator iter0 = jDoc["data"].MemberBegin(); iter0 != jDoc["data"].MemberEnd(); iter0++) {
													std::string path = iter0->name.GetString();
													
													if (iter0->value.HasMember("indexes")) {
														JsonDecode(iter0->value["indexes"], indexes);
													}

													if (iter0->value.HasMember("broadcast")) {
													//	printf("cnt: %d\n", m_stBroadcastData[app_id][path].size());
													//	for (std::map<int, std::string>::iterator iter = m_stBroadcastData[app_id][path].begin(); iter != m_stBroadcastData[app_id][path].end(); iter++) {
													//		printf("%d %s\n", iter->first, iter->second.data());
													//	}
													//	printf("end\n");
														int deletable = m_stBroadcastData[app_id][path].size() - 12;

														if (deletable < 0) deletable = 0;

														for (std::map<int, std::string>::const_iterator iter = m_stBroadcastData[app_id][path].begin(); iter != m_stBroadcastData[app_id][path].end() && deletable > 0; ) {
															if (--deletable > 0) {
																iter = m_stBroadcastData[app_id][path].erase(iter);
															}
														}
													//	printf("cnt: %d\n", m_stBroadcastData[app_id][path].size());
													//	for (std::map<int, std::string>::iterator iter = m_stBroadcastData[app_id][path].begin(); iter != m_stBroadcastData[app_id][path].end(); iter++) {
													//		printf("%d %s\n", iter->first, iter->second.data());
													//	}
													//	printf("end\n");
														m_stBroadcastData[app_id][path][++m_nBroadcastSerial] = iter0->value["broadcast"].GetString();
													}
													if (iter0->value.HasMember("answer")) {
														for (Value::ConstValueIterator iter = iter0->value["answer"].Begin(); iter != iter0->value["answer"].End(); iter++) {
															m_stDialogData[app_id][path][(*iter)["idx"].GetInt()].clear();
															m_stDialogData[app_id][path][(*iter)["idx"].GetInt()]["answer"] = (*iter)["answer"].GetString();
														}
													}

													for(std::map<std::string, std::map<int, std::map<std::string, std::string>>>::const_iterator iter = m_stDialogData[app_id].begin(); iter != m_stDialogData[app_id].end(); iter++) {
														for(std::map<int, std::map<std::string, std::string>>::const_iterator iter2 = iter->second.begin(); iter2 != iter->second.end(); iter2++) {
															if (!FindIndex(indexes, iter2->first)) {
																for(std::map<std::string, std::string>::const_iterator iter3 = iter2->second.begin(); iter3 != iter2->second.end(); iter3++) {
																	if (iter3->first == "ask") {
																		Value o(kObjectType);
																		o.AddMember("idx", iter2->first, a);
																		set_string(o,"text",iter3->second, a);
																		jArr.PushBack(o, a);
																	}
																}
															}
														}
													}			
													if (!jRes.HasMember("ask")) jRes.AddMember("ask", Value(kObjectType),a);
													set_value(jRes["ask"], path.data(), jArr, a);
												}
											}
										}
										break;
									case ACT_RES_CLIENT:

										if (jDoc.HasMember("path") && jDoc.HasMember("ask")) {
											std::string path = jDoc["path"].GetString();

											if (jDoc.HasMember("indexes")) {
												JsonDecode(jDoc["indexes"], indexes);
											}

											if (jDoc.HasMember("ask")) {
												m_stDialogData[app_id][path][++m_nDialogSerial]["ask"] = jDoc["ask"].GetString();
												jRes.AddMember("ticket", m_nDialogSerial, a);
											}

											if (jDoc.HasMember("tickets")) {
												const Value & jTickets = jDoc["tickets"];
												Value jAns(kArrayType);

												for (Value::ConstValueIterator iter = jTickets.Begin(); iter != jTickets.End(); iter++) {
													if (m_stDialogData[app_id][path][(*iter).GetInt()].find("answer") != m_stDialogData[app_id][path][(*iter).GetInt()].end()) {
														Value o(kObjectType), o2;
														std::string s = m_stDialogData[app_id][path][(*iter).GetInt()]["answer"];
														
														o.AddMember("idx", -1, a);
														set_string(o,"text",s, a);

														jArr.PushBack(o, a);
														jAns.PushBack((*iter).GetInt(), a);

														std::map<int, std::map<std::string, std::string>>::const_iterator iter2 = m_stDialogData[app_id][path].find((*iter).GetInt());
														if (iter2 != m_stDialogData[app_id][path].end())
															m_stDialogData[app_id][path].erase(iter2);
													};
												}
												jRes.AddMember("answered", jAns, a);
											}

											for (std::map<int, std::string>::const_iterator iter = m_stBroadcastData[app_id][path].begin(); iter != m_stBroadcastData[app_id][path].end(); iter++) {
												if (!FindIndex(indexes, iter->first)) {
													Value o(kObjectType);

													o.AddMember("idx", iter->first, a);
													set_string(o,"text",iter->second, a);
													jArr.PushBack(o, a);
												}
											}
											jRes.AddMember("data", jArr, a);
										}
										break;
									}
								}
							}
							m_mutex.unlock();
						}
					}
				}

				STRINGIFY(jRes, buffer);

				ws.sendFrame(buffer.GetString(), buffer.GetSize(), flags); 
			}
			while (n > 0 && (flags & WebSocket::FRAME_OP_BITMASK) != WebSocket::FRAME_OP_CLOSE);
			app.logger().information("WebSocket connection closed.");
		}
		catch (WebSocketException& exc)
		{
			app.logger().log(exc);
			switch (exc.code())
			{
			case WebSocket::WS_ERR_HANDSHAKE_UNSUPPORTED_VERSION:
				response.set("Sec-WebSocket-Version", WebSocket::WEBSOCKET_VERSION);
				// fallthrough
			case WebSocket::WS_ERR_NO_HANDSHAKE:
			case WebSocket::WS_ERR_HANDSHAKE_NO_VERSION:
			case WebSocket::WS_ERR_HANDSHAKE_NO_KEY:
				response.setStatusAndReason(HTTPResponse::HTTP_BAD_REQUEST);
				response.setContentLength(0);
				response.send();
				break;
			}
		}
	}   

private:

	static std::mutex 		m_mutex;
	static int				m_nDialogSerial;
	static int				m_nBroadcastSerial;
	static std::map<int, std::string> 	m_stARCSApps;
	static std::map<int, std::map<std::string, std::map<int, std::string>>>	m_stBroadcastData;
	static std::map<int, std::map<std::string, std::map<int, std::map<std::string, std::string>>>>	m_stDialogData;
};

std::mutex 		WebSocketRequestHandler::m_mutex;
int				WebSocketRequestHandler::m_nDialogSerial = 0;
int				WebSocketRequestHandler::m_nBroadcastSerial = 0;
std::map<int, std::string>	WebSocketRequestHandler::m_stARCSApps;
std::map<int, std::map<std::string, std::map<int, std::string>>>	WebSocketRequestHandler::m_stBroadcastData;
std::map<int, std::map<std::string, std::map<int, std::map<std::string, std::string>>>>	WebSocketRequestHandler::m_stDialogData;

class RequestHandlerFactory: public HTTPRequestHandlerFactory
{
public:
	HTTPRequestHandler* createRequestHandler(const HTTPServerRequest& request)
	{
		return new WebSocketRequestHandler;
	}
};

class WebSocketServer: public Poco::Util::ServerApplication
{
public:
	WebSocketServer(): _helpRequested(false)
	{
	}
	
	~WebSocketServer()
	{
	}

protected:
	void initialize(Application& self)
	{
		loadConfiguration(); // load default configuration files, if present
		ServerApplication::initialize(self);
	}
		
	void uninitialize()
	{
		ServerApplication::uninitialize();
	}

	void defineOptions(OptionSet& options)
	{
		ServerApplication::defineOptions(options);
		
		options.addOption(
			Option("help", "h", "display help information on command line arguments")
				.required(false)
				.repeatable(false));
	}

	void handleOption(const std::string& name, const std::string& value)
	{
		ServerApplication::handleOption(name, value);

		if (name == "help")
			_helpRequested = true;
	}

	void displayHelp()
	{
		HelpFormatter helpFormatter(options());
		helpFormatter.setCommand(commandName());
		helpFormatter.setUsage("OPTIONS");
		helpFormatter.setHeader("A sample HTTP server supporting the WebSocket protocol.");
		helpFormatter.format(std::cout);
	}

	int main(const std::vector<std::string>& args)
	{
		if (_helpRequested)
		{
			displayHelp();
		}
		else
		{
			// get parameters from configuration file
			unsigned short port = (unsigned short) config().getInt("WebSocketServer.port", 8080);
			
			// set-up a server socket
			ServerSocket svs(port);
			// set-up a HTTPServer instance
			HTTPServer srv(new RequestHandlerFactory, svs, new HTTPServerParams);

			// start the HTTPServer
			srv.start();
			// wait for CTRL-C or kill
			waitForTerminationRequest();
			// Stop the HTTPServer
			srv.stop();
		}
		return Application::EXIT_OK;
	}
	
private:
	bool _helpRequested;
};

POCO_SERVER_MAIN(WebSocketServer)


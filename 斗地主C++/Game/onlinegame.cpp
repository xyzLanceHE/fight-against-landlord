﻿#include "onlinegame.h"
#include <algorithm>
#include <thread>
#include <iostream>
#include <fstream>
#include <sstream>
#include "json.h"

using namespace std::string_literals;

namespace PokerGame
{
	namespace FAL
	{

		void OnlineServer::Start()
		{
			auto broadcastSceneThread = std::thread(&OnlineServer::BroadCastSceneThread, this);
			broadcastSceneThread.detach();
			auto serviceListenThread = std::thread(&OnlineServer::ListenThread, this);
			broadcastSceneThread.detach();
		}

		void OnlineServer::Reset()
		{
			this->resetFlag = 0b1;
			while (this->resetFlag)
			{
				using namespace std::chrono_literals;
				std::this_thread::sleep_for(1s);
			}
			this->GameReset();
		}

		void OnlineServer::Init()
		{
			try
			{
				this->NetInit();
				this->GameReset();
			}
			catch (ServerInitFailedException&)
			{
				throw;
			}
		}

		OnlineServer::OnlineServer()
		{
			this->multicastPort = 6666;
			this->multicastLocalBindPort = 6660;
			this->multicastIP = "239.0.1.10"s;
			this->multicastLocalBindIP = "192.168.1.102"s;
			this->serviceListenPort = 6665;
		}

		OnlineServer::OnlineServer(std::string configPath)
		{
			Json::CharReaderBuilder readerBuilder;
			Json::Value config;
			std::unique_ptr<Json::CharReader> const reader(readerBuilder.newCharReader());

			std::ifstream fileIn(configPath.c_str(), std::ios::binary);
			if (!fileIn.is_open())
			{
				throw std::exception();
			}
			std::stringstream configSS;
			std::string configJsonStr;
			std::string parseErr;
			configSS << fileIn.rdbuf();
			configJsonStr = configSS.str();

			reader->parse(configJsonStr.c_str(), configJsonStr.c_str() + configJsonStr.length(), &config, &parseErr);
			
			this->multicastPort = config["MulticastPort"].asInt();
			this->multicastIP = config["MulticastIP"].asString();
			this->multicastLocalBindPort = config["MulticastLocalBindPort"].asInt();
			this->multicastLocalBindIP = config["MulticastLocalBindIP"].asString();
			this->serviceListenPort = config["ServiceListenPort"].asInt();
		}

		void OnlineServer::BroadCastSceneThread()
		{
			while (true)
			{
				int reset = this->resetFlag & 0b1;
				if (reset)
				{
					this->resetFlag &= 0b0;
					return;
				}

				Scene current = this->FormCurrentScene();
				int sentlen = sendto(this->broadcastSocketFD,
					reinterpret_cast<char*>(&current),
					sizeof(Scene),
					0,
					reinterpret_cast<sockaddr*>(&this->clientBroadCastAddr),
					sizeof(sockaddr_in));
				using namespace std::chrono_literals;
				if (sentlen == -1)
				{
					std::cout << "broadcast thread dead"s << std::endl;
					break;
				}
				std::this_thread::sleep_for(0.5s);
			}
		}

		void OnlineServer::ListenThread()
		{
			while (true)
			{
				#pragma message("注意回收内存")
				char* buffer = new char[100];
				sockaddr_in clientIP = {0};
				int nameSize = sizeof(sockaddr_in);
				int rcvdLen = 
					recvfrom(this->serviceSocketFD, 
						buffer, 
						100, 
						0, 
						reinterpret_cast<sockaddr*>(&clientIP), 
						&nameSize);
				if (rcvdLen == -1)
				{
					int err = WSAGetLastError();
					//TODO : 考虑更多错误代码的处理方式
					switch (err)
					{
					case WSAEMSGSIZE:
						break;
					default:
						std::cout << "service thread dead:" << err;
						return;
					}
				}
				auto msgProcessThread = std::thread(&OnlineServer::HandleMessage, this, buffer, rcvdLen, clientIP);
				msgProcessThread.detach();
			}
		}

		void OnlineServer::NetInit()
		{

			// 初始化组播套接字
			this->broadcastSocketFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
			if (this->broadcastSocketFD == INVALID_SOCKET)
			{
				std::cout << WSAGetLastError();
				throw ServerInitFailedException();
			}
			this->clientBroadCastAddr.sin_family = AF_INET;
			this->clientBroadCastAddr.sin_port = htons(this->multicastPort);
			sockaddr_in localBind;
			localBind.sin_family = AF_INET;
			inet_pton(AF_INET, this->multicastLocalBindIP.c_str(), &localBind.sin_addr.s_addr);
			localBind.sin_port = htons(this->multicastLocalBindPort);
			int bindRet = bind(this->broadcastSocketFD, reinterpret_cast<sockaddr*>(&localBind), sizeof(sockaddr_in));
			if (bindRet != 0)
			{
				std::cout << WSAGetLastError();
				throw ServerInitFailedException();
			}
			inet_pton(AF_INET, this->multicastIP.c_str(), &this->clientBroadCastAddr.sin_addr.s_addr);
		
			// 初始化收听套接字
			this->serviceSocketFD = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
			if (this->serviceSocketFD == INVALID_SOCKET)
			{
				throw ServerInitFailedException();
			}
			sockaddr_in serviceListenAddr;
			serviceListenAddr.sin_family = AF_INET;
			serviceListenAddr.sin_addr.s_addr = INADDR_ANY;
			serviceListenAddr.sin_port = htons(this->serviceListenPort);
			bindRet = bind(this->serviceSocketFD, reinterpret_cast<sockaddr*>(&serviceListenAddr), sizeof(sockaddr_in));
			if (bindRet != 0)
			{
				std::cout << WSAGetLastError();
				throw ServerInitFailedException();
			}
		}

		void OnlineServer::GameReset()
		{
			this->gameStage = STAGE_PREPARING;
			ClientID playerIDs[3];
			this->isLandlordDetermined = false;
			std::fill(std::begin(this->landlordWillingness), std::end(this->landlordWillingness), '\0');
			this->landlordIndex = 0;
			this->nextActPlayerIndex = -1;
			this->nextActType = ACTIVE_TYPE_ACTIVE;
			this->lastActType = ACTIVE_TYPE_PASS;
			this->secondLastActType = ACTIVE_TYPE_PASS;
			this->winnerFlag = 0b000;
			this->lastAct.reset(new IdBasedCardCollection());
			this->secondLastAct.reset(new IdBasedCardCollection());
			for (int i = 0; i < 3; i++)
			{
				this->playerCards[i].reset(new IdBasedCardCollection());
			}
			this->lordCards.reset(new IdBasedCardCollection());
		}

		void OnlineServer::HandleMessage(char* buf, int bufLen, sockaddr_in addr)
		{
			if (bufLen < 2 * sizeof(int))
			{
				delete[] buf;
				return;
			}
			int* pIntBuf = reinterpret_cast<int*>(buf);
			int nameHash = pIntBuf[0];
			int opertionType = pIntBuf[1];
			int playerIndex = this->FindPlayerIndex(nameHash, addr);
			if (playerIndex == -1)
			{

			}
			else
			{

			}
			delete[] buf;
		}

		int OnlineServer::FindPlayerIndex(int nameHash, sockaddr_in addr) noexcept
		{
			ClientID clientID;
			clientID.nameHashCode = nameHash;
			clientID.ip = addr;
			for (int index = 0; index < 3; index++)
			{
				auto joinedClient = *const_cast<ClientID*>(&this->playerIDs[index]);
				if (clientID == joinedClient)
				{
					return index;
				}
			}
			return -1;
		}

		Scene OnlineServer::FormCurrentScene() noexcept
		{
			Scene currentScene;
			currentScene.CurrentStage = static_cast<char>(this->gameStage);
			this->lastAct->Serialize(currentScene.LastCardDrop, 20);
			this->secondLastAct->Serialize(currentScene.SecondLastCardDrop, 20);
			currentScene.ActiveIndex = static_cast<char>(this->nextActPlayerIndex);
			switch (this->gameStage)
			{
			case STAGE_DETERMINE_LANDLORD:
			{
				auto maxWill = std::max_element(std::begin(this->landlordWillingness), std::end(this->landlordWillingness));
				currentScene.ActiveParam = *maxWill + 1;
				break;
			}
			case STAGE_MAINLOOP_ONGOING:
			{
				currentScene.ActiveParam = this->nextActType;
				break;
			}
			default:
				currentScene.ActiveParam = '\0';
				break;
			}
			currentScene.ActiveParam = this->nextActType;
			for (int i = 0; i < 3; i++)
			{
				currentScene.CardLeftCount[i] = static_cast<char>(this->playerCards[i]->Count());
			}
			this->lordCards->Serialize(currentScene.LandlordCards, 3);
			if (this->isLandlordDetermined)
			{
				currentScene.LandlordIndex = static_cast<char>(this->landlordIndex);
			}
			else
			{
				currentScene.LandlordIndex = static_cast<char>(-1);
			}
			std::copy(std::begin(this->landlordWillingness), 
				std::end(this->landlordWillingness), 
				std::begin(currentScene.LandlordWillingness));
			currentScene.LastActType = this->lastActType;
			currentScene.SecondLastActType = this->secondLastActType;
			currentScene.WinnerFlag = static_cast<char>(this->winnerFlag);
			return currentScene;
		}
	}
}
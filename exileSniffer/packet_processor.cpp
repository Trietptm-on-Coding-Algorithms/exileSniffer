#include "stdafx.h"
#include "packet_processor.h"
#include "packetIDs.h"
#include "utilities.h"
#include "inventory.h"
#pragma comment(lib, "N:\\code\\POEcode\\poeSRE\\clientX\\packages\\cryptopp.v140.5.6.5.2\\lib\\native\\v140\\windesktop\\msvcstl\\x64\\Debug\\md\\cryptopp.lib")

std::string getSkillName(unsigned long skillID)
{
	return "TODOSKILLNAMES";
}

void packet_processor::handle_packet_from_patchserver(byte* data, unsigned int dataLen)
{
	char pktType = data[0];
	int i = 1;

	switch (pktType)
	{
	case 2:
	{
		std::cout << "\tServer info Response:" << std::endl;
		bool nullHash = true;
		for (i = 1; i < 33; ++i)
			if (data[i] != 0)
				nullHash = false;

		std::cout << "\t\tHash:";
		if (nullHash) std::cout << "[Null]";
		else
		{
			for (i = 1; i < 33; ++i)
				std::cout << std::hex << std::setw(2) << data[i];
		}
		std::cout << std::endl;

		i++;
		byte url1Size = data[i++] * 2;
		std::wstring url1(reinterpret_cast<wchar_t*>(data + i), (url1Size) / sizeof(wchar_t));
		std::wcout << "\t\tServer: " << url1 << std::endl;


		i += (url1Size + 1);
		byte url2Size = data[i++] * 2;
		std::wstring url2(reinterpret_cast<wchar_t*>(data + i), (url2Size) / sizeof(wchar_t));
		std::wcout << "\t\tBackup Server: " << url2 << std::endl;

		return;
	}
	case 4:
		printf("\tFolder info response: <Effort>\n");
		return;

	case 6:
		printf("\tPatch note response: <Effort>\n");
		return;
	}
	printf("\tUnhandled pkt type: %d, len %ld\n", pktType, dataLen);
}


void packet_processor::handle_packet_to_patchserver(byte* data, unsigned int dataLen)
{
	char pktType = data[0];

	switch (pktType)
	{
	case 1:
		if (dataLen == 2)
		{
			printf("\tServer info request. Proto version: %x\n", (char)data[1]);
			return;
		}
		break;
	case 3:
		printf("\tFolder info request: %x%x%x\n", (char)data[1], (char)data[2], (char)data[3]);
		return;

	case 5:
		printf("\tPatch note request\n");
		return;

	}
	printf("\tUnhandled pkt type: %d, len %ld\n", pktType, dataLen);
}

void packet_processor::handle_patch_data(byte* data)
{
	char *next_token = (char *)data;

	char *streamID_s = strtok_s(next_token, ",", &next_token);
	networkStreamID streamID = (networkStreamID)atoi(streamID_s);

	char *incoming = strtok_s(next_token, ",", &next_token);
	currentMsgIncoming = (*incoming == '1') ? true : false;

	char *timeprocessed = strtok_s(next_token, ",", &next_token);
	long long msProcessed = atoll(timeprocessed);

	char *dLen = strtok_s(next_token, ",", &next_token);
	unsigned int payloadLen = atoi(dLen);

	byte *payload = (byte *)next_token;

	if (currentMsgIncoming)
	{
		printf("Server patch data (%ld bytes):\n", payloadLen);
		handle_packet_from_patchserver(payload, payloadLen);
	}
	else
	{
		printf("Client patch data (%ld bytes):\n", payloadLen);
		handle_packet_to_patchserver(payload, payloadLen);
	}
}





void packet_processor::handle_packet_from_loginserver(byte* data, unsigned int dataLen, long long timems)
{
	std::cout << "got loginpkt size " << std::dec << dataLen << std::endl;
	if (currentStreamObj->ephKeys < 2) //warning - a null packet is ignored
	{
		if (dataLen < 2) return;
		ushort pktID = ntohs(getUshort(data));
		if (pktID != LOGIN_EPHERMERAL_PUBKEY) return;

		currentStreamObj->ephKeys++;

		decryptedBuffer = new vector<byte>(data, data + dataLen);
		std::cout << "firsdbuf 0x" << std::hex << decryptedBuffer << std::endl;
		remainingDecrypted = dataLen;
		decryptedIndex = 0;

		UI_RAWHEX_PKT *hexmsg = new UI_RAWHEX_PKT(0, eLogin, true);
		hexmsg->setData(decryptedBuffer);
		uiMsgQueue->addItem(hexmsg);

		UIDecodedPkt *ui_decodedpkt = new UIDecodedPkt(0, eLogin, PKTBIT_INBOUND, timems);

		deserialisedPkts.push_back(ui_decodedpkt);
		ui_decodedpkt->setBuffer(decryptedBuffer);
		ui_decodedpkt->setStartOffset(0);
		ui_decodedpkt->setEndOffset(dataLen);
		ui_decodedpkt->messageID = LOGIN_EPHERMERAL_PUBKEY;
		ui_decodedpkt->toggle_payload_operations(true);

		decryptedIndex = 2;
		remainingDecrypted = dataLen - 2;
		packet_processor::deserialiser deserialiserForPktID = loginPktDeserialisers.at(LOGIN_EPHERMERAL_PUBKEY);
		(this->*deserialiserForPktID)(ui_decodedpkt);

		uiMsgQueue->addItem(ui_decodedpkt);

		return;
	}

	decryptedBuffer = new vector<byte>;
	decryptedBuffer->resize(dataLen, 0);

	bool alreadyDecrypted = false;

	if (!currentStreamObj->workingRecvKey)
	{
		while (!currentStreamObj->workingRecvKey)
		{
			KEYDATA *keyCandidate = keyGrabber->getUnusedMemoryKey(currentMsgStreamID, true);
			if (!keyCandidate) {
				std::cout << "no unused memkey in from login!" << std::endl;
				Sleep(1200);
				continue;
			}

			if (keyCandidate->used) std::cout << "assert 3" << std::endl;
			assert(!keyCandidate->used);

			currentStreamObj->fromLoginSalsa.SetKeyWithIV((const byte *)keyCandidate->salsakey,
				32,
				(const byte *)keyCandidate->IV);
			currentStreamObj->fromLoginSalsa.ProcessData(decryptedBuffer->data(), data, dataLen);


			unsigned short packetID = ntohs(getUshort(decryptedBuffer->data()));
			if (packetID == 0x0004)
			{
				alreadyDecrypted = true;
				keyCandidate->used = true;
				keyGrabber->claimKey(keyCandidate, currentMsgStreamID);

				UIaddLogMsg("Loginserver receive key recovered",
					keyCandidate->sourceProcess,
					uiMsgQueue);
				UIrecordLogin(keyCandidate->sourceProcess, uiMsgQueue);

				keyGrabber->stopProcessScan(keyCandidate->sourceProcess);
				currentStreamObj->workingRecvKey = keyCandidate;
				break;
			}
			else
			{
				std::stringstream err;
				err << "Error expected packet 0x4 from loginserver but got 0x" << std::hex << packetID;

				UIaddLogMsg(err.str(), 0, uiMsgQueue);

				//todo: need to handle gracefully

				return;
			}
		}

	}

	if(!alreadyDecrypted)
		currentStreamObj->fromLoginSalsa.ProcessData(decryptedBuffer->data(), data, dataLen);

	UI_RAWHEX_PKT *msg = new UI_RAWHEX_PKT(currentStreamObj->workingSendKey->sourceProcess, eLogin, true);
	msg->setData(decryptedBuffer);
	uiMsgQueue->addItem(msg);

	remainingDecrypted = dataLen;
	decryptedIndex = 0;

	deserialise_packets_from_decrypted(eLogin, PKTBIT_INBOUND, timems);
/*
	char pktType = decryptedBuffer->at(1);
	switch (pktType)
	{
	case LOGIN_SRV_NOTIFY_GAMESERVER:
	{
		std::cout << "Got gameserver info from loginserver" << std::endl;
		unsigned int pktidx = 2;

		unsigned long connectionID = ntohl(getUlong(&decryptedBuffer->at(10)));
		std::cout << "Got key for connection ID " << std::hex << connectionID << std::endl;

		pktidx = 17;
		unsigned int port = (decryptedBuffer->at(17) << 8) + decryptedBuffer->at(18);
		std::stringstream serverIP;
		serverIP << (int)decryptedBuffer->at(19) << ".";
		serverIP << (int)decryptedBuffer->at(20) << ".";
		serverIP << (int)decryptedBuffer->at(21) << ".";
		serverIP << (int)decryptedBuffer->at(22) << ":" << std::dec << port;
		std::cout << "\tGameserver: " << serverIP.str() << std::endl;

		DWORD *keyblob = (DWORD *)(&decryptedBuffer->at(43));


		KEYDATA *key1A = new KEYDATA;
		KEYDATA *key1B = new KEYDATA;

		key1A->salsakey[0] = key1B->salsakey[0] = keyblob[0];
		key1A->salsakey[1] = key1B->salsakey[1] = keyblob[1];
		key1A->salsakey[2] = key1B->salsakey[2] = keyblob[2];
		key1A->salsakey[3] = key1B->salsakey[3] = keyblob[3];
		key1A->salsakey[4] = key1B->salsakey[4] = keyblob[4];
		key1A->salsakey[5] = key1B->salsakey[5] = keyblob[5];
		key1A->salsakey[6] = key1B->salsakey[6] = keyblob[6];
		key1A->salsakey[7] = key1B->salsakey[7] = keyblob[7];
		key1A->IV[0] = keyblob[8];
		key1A->IV[1] = keyblob[9];
		key1B->IV[0] = keyblob[12];
		key1B->IV[1] = keyblob[13];

		if (key1A->salsakey[0] == 0 && key1A->salsakey[3] == 0 && key1A->salsakey[7])
		{
			std::cout << "Discarding bad key in play response" << std::endl;
			break; //probably an old zero-ed out key
		}

		key1A->sourceProcess = key1B->sourceProcess = currentStreamObj->workingRecvKey->sourceProcess;
		key1A->foundAddress = key1B->foundAddress = SENT_BY_SERVER;
		pendingGameserverKeys[connectionID] = make_pair(key1A, key1B);

		return;
	}

	case LOGIN_SRV_RACE_DATA:
		printf("Race description response: <effort>\n");
		return;

	case LOGIN_SRV_LEAGUE_LIST:
		std::cout << "League data from login server" << decryptedBuffer << std::endl;
		return;


	default:
		printf("Unknown packet from login server: byte[1]==0x%x (dec %d)\n", decryptedBuffer[1], decryptedBuffer[1]);

		std::cout << "Hex Payload: " << std::endl;
		for (int i = 0; i < dataLen; ++i)
		{
			byte item = decryptedBuffer->at(i);
			std::cout << std::hex << std::setw(2) << (int)item;
			if (i % 16 == 0) std::cout << std::endl;
		}
		std::cout << std::endl;
		return;
	}
	*/
}

void packet_processor::handle_packet_to_loginserver(byte* data, unsigned int dataLen, long long timems)
{
	std::cout << "got loginpkt size " << std::dec << dataLen << std::endl;
	if (currentStreamObj->ephKeys < 2)
	{
		if (dataLen < 2) return;
		ushort pktID = ntohs(getUshort(data));
		if (pktID != LOGIN_EPHERMERAL_PUBKEY) return;

		currentStreamObj->ephKeys++;

		decryptedBuffer = new vector<byte>(data, data + dataLen);
		remainingDecrypted = dataLen;
		decryptedIndex = 0;

		UI_RAWHEX_PKT *msg = new UI_RAWHEX_PKT(0, eLogin, false);
		
		msg->setData(decryptedBuffer);
		uiMsgQueue->addItem(msg);

		UIDecodedPkt *ui_decodedpkt = new UIDecodedPkt(0, eLogin, PKTBIT_OUTBOUND, timems);
		ui_decodedpkt->setBuffer(decryptedBuffer);
		ui_decodedpkt->setStartOffset(0);
		ui_decodedpkt->setEndOffset(dataLen);
		ui_decodedpkt->messageID = LOGIN_EPHERMERAL_PUBKEY;
		ui_decodedpkt->toggle_payload_operations(true);
		deserialisedPkts.push_back(ui_decodedpkt);

		decryptedIndex = 2;
		remainingDecrypted = dataLen - 2;
		packet_processor::deserialiser deserialiserForPktID = loginPktDeserialisers.at(LOGIN_EPHERMERAL_PUBKEY);
		(this->*deserialiserForPktID)(ui_decodedpkt);

		uiMsgQueue->addItem(ui_decodedpkt);

		return;
	}

	decryptedBuffer = new vector<byte>;
	decryptedBuffer->resize(dataLen, 0);

	if (!currentStreamObj->workingSendKey)
	{
		unsigned int msWaited = 0;
		std::cout << "Login request: " << std::endl;
		while (true)
		{

			//should use hint here, but have to reliably find corresponding stream
			KEYDATA *keyCandidate = keyGrabber->getUnusedMemoryKey(currentMsgStreamID, false);
			if (!keyCandidate) {
				Sleep(200);
				msWaited += 200;
				//every two seconds relax the memory scan filters
				if(msWaited % 2000 == 0)
					keyGrabber->relaxScanFilters();

				continue;
			}

			if (keyCandidate->used) std::cout << "assert 4" << std::endl;
			assert(!keyCandidate->used);

			currentStreamObj->toLoginSalsa.SetKeyWithIV((byte *)keyCandidate->salsakey,
				32,
				(byte *)keyCandidate->IV);
			currentStreamObj->toLoginSalsa.ProcessData(decryptedBuffer->data(), data, dataLen);

			if (decryptedBuffer->at(0) == 0 && decryptedBuffer->at(1) == 3)
			{
				keyCandidate->used = true;
				keyGrabber->claimKey(keyCandidate, currentMsgStreamID);

				UIaddLogMsg("Loginserver send key recovered", 
					keyCandidate->sourceProcess, 
					uiMsgQueue);

				currentStreamObj->workingSendKey = keyCandidate;
				break;
			}
		}
		
		//overwrite the creds before proceeding
		ushort namelen = ntohs(getUshort(&decryptedBuffer->at(6)));
		ushort credsloc = 2 + 4 + 2 + (namelen * 2) + 32;
		for (int i = 0; i < 32; i++)
			decryptedBuffer->at(credsloc+i) = 0xf;

		UI_RAWHEX_PKT *msg = new UI_RAWHEX_PKT(currentStreamObj->workingSendKey->sourceProcess, eLogin, false);
		msg->setData(decryptedBuffer);
		uiMsgQueue->addItem(msg);


		
		UIDecodedPkt *ui_decodedpkt = new UIDecodedPkt(0, eLogin, PKTBIT_OUTBOUND, timems);

		ui_decodedpkt->setBuffer(decryptedBuffer);
		ui_decodedpkt->setStartOffset(0);
		ui_decodedpkt->setEndOffset(dataLen);
		ui_decodedpkt->messageID = LOGIN_CLI_AUTH_DATA;
		ui_decodedpkt->toggle_payload_operations(true);
		deserialisedPkts.push_back(ui_decodedpkt);

		remainingDecrypted = dataLen - 2;
		decryptedIndex = 2;
		packet_processor::deserialiser deserialiserForPktID = loginPktDeserialisers.at(LOGIN_CLI_AUTH_DATA);
		(this->*deserialiserForPktID)(ui_decodedpkt);

		uiMsgQueue->addItem(ui_decodedpkt);
		
		return;
	}

	currentStreamObj->toLoginSalsa.ProcessData(decryptedBuffer->data(), data, dataLen);
	
	UI_RAWHEX_PKT *msg = new UI_RAWHEX_PKT(currentStreamObj->workingSendKey->sourceProcess, eLogin, false);
	msg->setData(decryptedBuffer);
	uiMsgQueue->addItem(msg);

	remainingDecrypted = dataLen;
	decryptedIndex = 0;

	deserialise_packets_from_decrypted(eLogin, PKTBIT_OUTBOUND, timems);
	/*
	unsigned short pktIDWord = ntohs(consumeUShort());
	std::cout << "pkid " << pktIDWord << std::endl;

	switch (pktIDWord)
	{
	case LOGIN_CLI_KEEP_ALIVE:
	{
		UIDecodedPkt *ui_decodedpkt =
			new UIDecodedPkt(currentStreamObj->workingSendKey->sourceProcess, eLogin, PKTBIT_OUTBOUND, timems);
		deserialisedPkts.push_back(ui_decodedpkt);
		ui_decodedpkt->setStartOffset(decryptedIndex - 2);
		ui_decodedpkt->messageID = pktIDWord;
		ui_decodedpkt->toggle_payload_operations(true);
		uiMsgQueue->addItem(ui_decodedpkt);

		std::cout << "Client sent KeepAlive pkt (0x01) to login server" << std::endl;
		return;
	}
	case LOGIN_CLI_CHARACTER_SELECTED_SELECTED:
	{
		printf("Play request\n");

		unsigned int charNameLen = decryptedBuffer->at(3) * 2;
		if (decryptedBuffer->at(2) != 0)
			printf("Warning, long login charname not handled\n");
		std::wstring charn(reinterpret_cast<wchar_t*>(decryptedBuffer + 4), (charNameLen) / sizeof(wchar_t));
		std::wcout << "\tSelected Char: " << charn << std::endl;
		//uidata.charname = charn;

		return;
	}
	case 11:
	{
		std::cout << "Character selection sent to login server by number: " << (int)decryptedBuffer->at(6) << std::endl;
		return;
	}
	default:
		printf("Client sent unknown packet to login server: 0x%x\n", decryptedBuffer->at(1));

		std::cout << "Hex Payload: " << std::endl;
		for (int i = 0; i < dataLen; ++i)
		{
			byte item = decryptedBuffer->at(i);
			std::cout << std::hex << std::setw(2) << (int)item;
			if (i % 16 == 0) std::cout << std::endl;
		}
		std::cout << std::endl;

		return;
	}
	*/

}

void packet_processor::handle_login_data(byte* data)
{

	char *next_token = (char *)data;

	char *streamID_s = strtok_s(next_token, ",", &next_token);
	currentMsgStreamID = (networkStreamID)atoi(streamID_s);

	char *incoming = strtok_s(next_token, ",", &next_token);
	currentMsgIncoming = (*incoming == '1') ? true : false;

	char *timeprocessed = strtok_s(next_token, ",", &next_token);
	long long msProcessed = atoll(timeprocessed);

	char *pLen = strtok_s(next_token, ",", &next_token);
	unsigned int payloadLen = atoi(pLen);
	if (!payloadLen)
		return;

	byte *payload = (byte *)next_token;

	currentStreamObj = &streamDatas[currentMsgStreamID];

	if (currentMsgIncoming)
	{
		handle_packet_from_loginserver(payload, payloadLen, msProcessed);
	}
	else
	{
		handle_packet_to_loginserver(payload, payloadLen, msProcessed);
	}


	++currentStreamObj->packetCount;
}

bool packet_processor::handle_game_data(byte* data)
{

	char *next_token = (char *)data;

	char *streamID_s = strtok_s(next_token, ",", &next_token);
	currentMsgStreamID = (networkStreamID)atoi(streamID_s);
	currentStreamObj = &streamDatas[currentMsgStreamID];

	char *incoming = strtok_s(next_token, ",", &next_token);
	currentMsgIncoming = (*incoming == '1');

	//the keys are established during handling of the first packet to the gameserver
	//sometimes we will process the response from the gameserver first, causing badness.
	//this delays processing of recv data until we have a recv key
	if (currentMsgIncoming && currentStreamObj->workingRecvKey == NULL)
		return false;

	char *timeprocessed = strtok_s(next_token, ",", &next_token);
	long long msProcessed = atoll(timeprocessed);

	char *pLen = strtok_s(next_token, ",", &next_token);
	unsigned int payloadLen = atoi(pLen);
	if (!payloadLen)
		return true;

	//std::cout << "handling game data, size "<<std::dec <<dataLen<<" incoming- "<<(int)isIncoming << std::endl;

	byte *payload = (byte *)next_token;

	if (payloadLen > 0)
	{
		if (currentMsgIncoming)
			handle_packet_from_gameserver(payload, payloadLen, msProcessed);
		else
			handle_packet_to_gameserver(payload, payloadLen, msProcessed);
	}

	++currentStreamObj->packetCount;
	return true;
}

bool packet_processor::sanityCheckPacketID(unsigned short pktID)
{
	if (!pktID || pktID > 0x220)
	{
		errorFlag = eDecodingErr::eBadPacketID;
		return false;
	}
	return true;
}


void packet_processor::deserialise_packets_from_decrypted(streamType streamServer, byte isIncoming, long long timeSeen)
{
	unsigned int dataLen = remainingDecrypted;
	while (remainingDecrypted > 0)
	{
		unsigned short pktIDWord = ntohs(consumeUShort());

		UIDecodedPkt *ui_decodedpkt =
			new UIDecodedPkt(currentStreamObj->workingSendKey->sourceProcess, streamServer, isIncoming, timeSeen);

		deserialisedPkts.push_back(ui_decodedpkt);
		ui_decodedpkt->setStartOffset(decryptedIndex - 2);
		ui_decodedpkt->messageID = pktIDWord;
		ui_decodedpkt->toggle_payload_operations(true);

		if (sanityCheckPacketID(pktIDWord) && errorFlag == eNoErr)
		{
			//find and run deserialiser for this packet
			map<unsigned short, deserialiser>* deserialiserList;
			if (streamServer == streamType::eGame)
				deserialiserList = &gamePktDeserialisers;
			else
				deserialiserList = &loginPktDeserialisers;

			auto it = deserialiserList->find(pktIDWord);
			if (it != deserialiserList->end())
			{
				packet_processor::deserialiser deserialiserForPktID = it->second;
				(this->*deserialiserForPktID)(ui_decodedpkt);

				if (errorFlag == eNoErr || errorFlag == eAbandoned)
				{
					ui_decodedpkt->setEndOffset(decryptedIndex);

					if (errorFlag == eAbandoned)
					{
						errorFlag = eNoErr;
						ui_decodedpkt->setAbandoned();
					}
				}
			}
			else
			{
				std::cout << "Unhandled Hex Payload msgID <gamesrv in> 0x" << std::hex << pktIDWord << std::endl;
				for (int i = 0; i < dataLen; ++i)
				{
					byte item = decryptedBuffer->at(i);
					std::cout << std::setw(2) << (int)item;
					if (i % 16 == 0) std::cout << std::endl;
				}
				std::cout << std::endl;

				errorFlag = eDecodingErr::ePktIDUnimplemented;
			}
		}
		
		//warning - can be resized during deserialisation - only store pointers afterwards
		ui_decodedpkt->setBuffer(decryptedBuffer);

		if (errorFlag != eNoErr)
		{
			remainingDecrypted = 0;
			emit_decoding_err_msg(pktIDWord, currentStreamObj->lastPktID);
			ui_decodedpkt->setFailedDecode();
			ui_decodedpkt->setEndOffset(dataLen);
		}

		if (currentMsgMultiPacket)
		{
			ui_decodedpkt->setMultiPacket();
			currentMsgMultiPacket = false;
		}
		uiMsgQueue->addItem(ui_decodedpkt);
		currentStreamObj->lastPktID = pktIDWord;
	}
}

void packet_processor::handle_packet_to_gameserver(byte* data, 
	unsigned int dataLen, long long timems)
{
	currentStreamObj = &streamDatas.at(currentMsgStreamID);
	if (currentStreamObj->workingSendKey == NULL)
	{
		if (data[0] == 0 && data[1] == 3)
		{
			unsigned long connectionID = ntohl(getUlong(data + 2));

			if (pendingGameserverKeys.find(connectionID) == pendingGameserverKeys.end())
			{
				if (!pendingGameserverKeys.empty())
				{
					std::cout << "no pending for conid but found a key... trying";
					currentStreamObj->workingSendKey = pendingGameserverKeys.begin()->second.first;
					currentStreamObj->workingRecvKey = pendingGameserverKeys.begin()->second.second;
					pendingGameserverKeys.clear();
				}
				else
				{
					UIaddLogMsg("Error: No pending gameserver key", activeClientPID, uiMsgQueue);
					return;
				}
			}
			else
			{
				currentStreamObj->workingSendKey = pendingGameserverKeys.at(connectionID).first;
				currentStreamObj->workingRecvKey = pendingGameserverKeys.at(connectionID).second;
			}
			
			currentStreamObj->toGameSalsa.SetKeyWithIV(
				(byte *)currentStreamObj->workingSendKey->salsakey, 32,
				(byte *)currentStreamObj->workingSendKey->IV);

			
			currentStreamObj->toGameSalsa.SetKeyWithIV(
				(byte *)currentStreamObj->workingSendKey->salsakey, 32,
				(byte *)currentStreamObj->workingSendKey->IV);

			pendingGameserverKeys.erase(connectionID);

			//todo: decoded ui packet

			UI_RAWHEX_PKT *msg = new UI_RAWHEX_PKT(
				currentStreamObj->workingSendKey->sourceProcess, eGame, false);
			vector<byte> *plainTextBuf = new vector<byte>(data, data+dataLen);
			msg->setData(plainTextBuf);
			uiMsgQueue->addItem(msg);

		}
		else
		{
			std::cerr << "Warning! Unexpected first client packet|||!!!" << std::endl;
		}
		return;
	}

	/*
	this is not deleted because pointers to it are held by the
	raw decoder and pointers into it are held by the decodedpackets. 
	Don't want to make a bunch of copies because it will balloon in size with long running streams
	*/
	decryptedBuffer = new vector<byte>;
	decryptedBuffer->resize(dataLen, 0);
	currentStreamObj->toGameSalsa.ProcessData(decryptedBuffer->data(), data, dataLen);



	UI_RAWHEX_PKT *msg = new UI_RAWHEX_PKT(currentStreamObj->workingSendKey->sourceProcess, eGame, false);
	msg->setData(decryptedBuffer);
	if (errorFlag != eNoErr)
		msg->setErrorIndex(decryptedIndex);
	uiMsgQueue->addItem(msg);

	remainingDecrypted = dataLen;
	decryptedIndex = 0;
	errorFlag = eNoErr;

	deserialise_packets_from_decrypted(eGame, false, timems);

}

void packet_processor::handle_packet_from_gameserver(byte* data, unsigned int dataLen, long long timems)
{
	currentStreamObj = &streamDatas.at(currentMsgStreamID);

	decryptedBuffer = new vector<byte>;
	decryptedBuffer->resize(dataLen, 0);
	decryptedIndex = 0;
	errorFlag = eNoErr;

	if (currentStreamObj->packetCount == 1)
	{
		//first packet from gameserver starts 0005, followed by crypt which starts 0012
		ushort firstPktID = ntohs(getUshort(data));
		assert(firstPktID == SRV_PKT_ENCAPSULATED);

		currentStreamObj->fromGameSalsa.SetKeyWithIV(
			(byte *)currentStreamObj->workingRecvKey->salsakey, 32,
				(byte *)currentStreamObj->workingRecvKey->IV);

		dataLen -= 2;
		currentStreamObj->fromGameSalsa.ProcessData(decryptedBuffer->data(), data+2, dataLen);
	}
	else
	{
		currentStreamObj->fromGameSalsa.ProcessData(decryptedBuffer->data(), data, dataLen);
	}

	//print the whole blob in the raw log
	UI_RAWHEX_PKT *msg = new UI_RAWHEX_PKT(currentStreamObj->workingRecvKey->sourceProcess, eGame, true);
	msg->setData(decryptedBuffer);
	if (errorFlag != eNoErr) msg->setErrorIndex(0);
	uiMsgQueue->addItem(msg);

	remainingDecrypted = dataLen;

	deserialise_packets_from_decrypted(eGame, PKTBIT_INBOUND, timems);
}


bool packet_processor::process_packet_loop()
{
	std::vector<byte> pkt;

	while (true)
	{
		//highest priority - only check patch/login if game is quiet
		if (checkPipe(gamepipe, &pendingPktQueue))
		{
			bool done = false;
			while (!pendingPktQueue.empty())
			{
				pkt = pendingPktQueue.front();
				done = handle_game_data(pkt.data());
				if(done)
					pendingPktQueue.pop_front();
				else
				{
					//tried to handle first response before first send so no key to read recv
					//wait until first send done
					while (!done)
					{
						Sleep(100);
						checkPipe(gamepipe, &pendingPktQueue);

						auto it = pendingPktQueue.begin();
						for (; it != pendingPktQueue.end(); it++)
						{
							pkt = *it;
							done = handle_game_data(pkt.data());
							if (done)
								break;
						}

						if (done)
							pendingPktQueue.erase(it);
					}
				}
			}
			continue;
		}

		if (checkPipe(loginpipe, &pendingPktQueue))
		{
			while (!pendingPktQueue.empty())
			{
				pkt = pendingPktQueue.front();
				handle_login_data(pkt.data());
				pendingPktQueue.pop_front();
			}
			continue;
		}

		if (checkPipe(patchpipe, &pendingPktQueue))
		{
			while (!pendingPktQueue.empty())
			{
				pkt = pendingPktQueue.front();
				handle_patch_data(pkt.data());
				pendingPktQueue.pop_front();
			}
		}
		else
			Sleep(10);
	}
	return true;
}




void packet_processor::main_loop()
{
	init_loginPkt_deserialisers();
	init_gamePkt_deserialisers();

	unsigned int errCount = 0;

	//pipes were implemented when the pcap was done in another process
	//todo: use something else now
	while (!patchpipe)	{
		patchpipe = connectPipe(L"\\\\.\\pipe\\patchpipe");
		Sleep(200);
		if (errCount++ > 10) break;
	}

	while (!loginpipe)	{
		loginpipe = connectPipe(L"\\\\.\\pipe\\loginpipe");
		Sleep(200);
		if (errCount++ > 10) break;
	}

	while (!gamepipe)	{
		gamepipe = connectPipe(L"\\\\.\\pipe\\gamepipe");
		Sleep(200);
		if (errCount++ > 10) break;
	}

	if (loginpipe && gamepipe)
	{
		process_packet_loop();
	}
	else
	{
		UIaddLogMsg("ERROR: Unable to connect to our own packet capture pipes. Failing.", activeClientPID, uiMsgQueue);
	}
}
/*
 * Messaging.h
 *
 *  Created on: Dec 8, 2015
 *      Author: yupeng
 */

#ifndef MESSAGING_H_
#define MESSAGING_H_

#include <string>
#include <pthread.h>

#include "MessageType.h"
using std::string;

string END_OF_MESSAGE = "\aEND_OF_MESSAGE\a";
string FILE_BLOCK_REQUEST_DELIMITATION = "\aFILE_BLOCK_REQUEST\a";

enum ListenStatus {
	NA,
	SUCCESS,
	FAILURE
};

class Messaging;
struct ThreadData {
	Messaging &mess;
	int local_port;
	string ip;
	int msgType;
	string msgContent;
	int client_sockfd;

	ThreadData(Messaging &mess, int port, string ip,
			int msgType, string msgContent, int client_sockfd)
	: mess(mess), local_port(port), ip(ip), msgType(msgType),
	  msgContent(msgContent), client_sockfd(client_sockfd) { }
};

void* messageHandler(void *fd);

class Messaging {
public:
	virtual ~Messaging();

	/*
	 send message in a new thread.
	*/
	bool sendMessage(string addr, int targetPort, int msgType, string msg);

	bool sendMessageForReply(string addr, int targetPort, int msgType, string msg, string &reply);

	/*
	 listen a port.
	 call messageReceived in new threads when valid messages arrive.
	 return: true or false(port in use).
	*/
	void listenMessage(int listenPort);
	int getListenStatus();

	virtual void messageReceived(int localListenPort, string fromHost, int msgType, string msg) = 0;

	pthread_mutex_t mutex_listen_status;

private:
	int listenStatus;

};


#endif /* MESSAGING_H_ */

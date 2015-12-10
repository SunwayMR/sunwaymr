/*
 * SunwayMRHelper.h
 *
 *  Created on: Dec 8, 2015
 *      Author: yupeng
 */

#include <iostream>
#include <sstream>
#include <fstream>
#include <cstdlib>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>

#include "SunwayMRHelper.h"

using namespace std;

std::vector<std::string> &split(const std::string &s, char delim, std::vector<std::string> &elems) {
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        elems.push_back(item);
    }
    return elems;
}

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, elems);
    return elems;
}

struct MatchPathSeparator
{
    bool operator()( char ch ) const
    {
        return ch == '\\' || ch == '/';
    }
};

string basename(string const& pathname )
{
	return pathname.substr(pathname.find_last_of("/\\") + 1);
}

struct thread_data{
   SunwayMRHelper *helper;
   const char *msg;
   int *v;
};

void *sendHostResourceInfoToMasterRepeatedly(void *data) {
	thread_data *my_data;
	my_data = (struct thread_data *)data;

	while(true) {
		sleep(3); // send host resource info every 3 seconds
		my_data->helper->sendHostResourceInfoToMaster(string(my_data->msg));
	}

	pthread_exit(NULL);
}

SunwayMRHelper::SunwayMRHelper(string masterAddr, int masterListenPort, int threads, int memory)
:masterAddr(masterAddr), masterListenPort(masterListenPort), threads(threads), memory(memory) {
	localAddr = getLocalHost();
	if (localAddr == "") {
		logError("failed to obtain local IP address.");
		exit(1);
	}
	listening = init();
	if (!listening) {
		logError("failed to start listening.");
		exit(1);
	}
}

SunwayMRHelper::~SunwayMRHelper() {

}

void SunwayMRHelper::setLocalResouce(int threads, int memory) {
	this->threads = threads;
	this->memory = memory;

	// send repeatedly
	stringstream ss;
	ss << threads << " " << memory << " " << listenPort;

	pthread_t thread;
	int v = 0;
	thread_data data = {
			this,
			ss.str().c_str(),
			&v
	};
	int rc = pthread_create(&thread, NULL, sendHostResourceInfoToMasterRepeatedly, (void *)&data);
	if (rc){
		logError("failed to create thread to send host resource info.");
		exit(1);
	}
}

void SunwayMRHelper::sendHostResourceInfoToMaster(string msg) {
	Messaging::sendMessage(masterAddr, masterListenPort, HOST_RESOURCE_INFO, msg);
}

void SunwayMRHelper::runApplication(string filePath, bool localMode) {
	// run user application
	string targetHostsFileName = allHostsFileName;
	if (localMode) targetHostsFileName = localHostFileName;

	int appUID = getCurrentTime();
	int fileUID1 = appUID;
	int fileUID2 = appUID + 1;

	string appFileName = basename(filePath);
	string hostsFileName = "hostsFile.lst";

	stringstream filePath2;
	filePath2 << fileSaveDir << targetHostsFileName;

	stringstream fileInfo1, fileInfo2;
	fileInfo1 << fileUID1 << " " << appUID << " " << appFileName;
	fileInfo2 << fileUID2 << " " << appUID << " " << hostsFileName;

	string fileContent1, fileContent2;
	bool rd1 = readFile(filePath, fileContent1);
	bool rd2 = readFile(filePath2.str(), fileContent2);

	if (!rd1 || !rd2) {
		exit(1);
	}

	// for all hosts in fileContent2
	stringstream ss(fileContent2);
	string line;
	vector<HostResource> tmp;

	bool sendWithFailure = false;
	while(std::getline(ss,line,'\n')){
		vector<string> v = split(line, ' ');
		if (v.size() == 4) {
			string host = v[0];
			int threads = atoi(v[1].c_str());
			int memory = atoi(v[2].c_str());
			int port = atoi(v[3].c_str());
			HostResource hr = {
					host,
					threads,
					memory,
					port
			};
			tmp.push_back(hr);

			// send file info 1
			bool sr = sendMessage(host, port, FILE_INFO, fileInfo1.str());
			if(!sr) {
				sendWithFailure = true;
				stringstream err;
				err << "failed to send file info[1] to host: " << host << ", " << port << "; info:" << fileInfo1.str();
				logError(err.str());
			}
			// send file info 2
			sr = sendMessage(host, port, FILE_INFO, fileInfo2.str());
			if(!sr) {
				sendWithFailure = true;
				stringstream err;
				err << "failed to send file info[2] to host: " << host << ", " << port << "; info:" << fileInfo2.str();
				logError(err.str());
			}

			// send file content 1
			sr = sendMessage(host, port, fileUID1, fileContent1);
			if(!sr) {
				sendWithFailure = true;
				stringstream err;
				err << "failed to send file content[1] to host: " << host << ", " << port;
				logError(err.str());
			}

			// send file content 2
			sr = sendMessage(host, port, fileUID2, fileContent2);
			if(!sr) {
				sendWithFailure = true;
				stringstream err;
				err << "failed to send file content[2] to host: " << host << ", " << port;
				logError(err.str());
			}
		}
	}

	if(sendWithFailure) exit(1);

	logInfo("sending files succeeded.");
	logInfo("starting...");

	string appExecutableName = split(appFileName, ',')[0];

	string masterValue = masterAddr;
	if (localMode) masterValue = localAddr;

	int appListenPort = randomValue(30001, 39999);

	stringstream startAppCmd;
	startAppCmd << "cd " << fileSaveDir << appUID << endl;
	startAppCmd << CXX << " -lstdc++ -pthread -fPIC -O2 -g -Wall -fmessage-length=0 -I../include -L../lib " << appFileName << " -o " << appExecutableName << endl;
	startAppCmd << "./" << appExecutableName << " " << hostsFileName << " " << masterValue << " " << appListenPort << endl;

	for(unsigned int i=0; i<tmp.size(); i++) {
		sendMessage(tmp[i].host, tmp[i].listenPort, SHELL_COMMAND, startAppCmd.str());
	}

}

bool SunwayMRHelper::init() {
	bool ret = false;

	if (localAddr == masterAddr) {
		listenPort = masterListenPort;
		ret = listenMessage(listenPort);

	} else {
		listenPort = randomValue(20001, 29999);
		ret = listenMessage(listenPort);

		int try_time = 0;
		while (!ret && try_time < 100) {
			try_time++;
			listenPort += 3;

			ret = listenMessage(listenPort);
		}
	}

	return ret;
}

void SunwayMRHelper::messageReceived(int localListenPort, string fromHost, int msgType, string msg) {
	if (localListenPort != this->listenPort || fromHost == "" || msg == "") return;

	switch(msgType) {
	case HOST_RESOURCE_INFO:
	{
		vector<string> ss = split(msg, ' ');
		if (ss.size() == 3) {
			HostResource hr = {
					fromHost,
					atoi(ss[0].c_str()),
					atoi(ss[1].c_str()),
					atoi(ss[2].c_str())
			};
			update(hr);
		}
		break;
	}

	case FILE_INFO:
	{
		vector<string> ss = split(msg, ' '); // msg: fileUID, appUID, fileName
		if (ss.size() == 3) {
			int file_uid = atoi(ss[0].c_str());
			fileInfoMap[file_uid] = msg;
		}
		break;
	}

	case SHELL_COMMAND:
	{
		int ret = system(msg.c_str());
		break;
	}

	default:
	{
		if(fileInfoMap.find(msgType) != fileInfoMap.end()) {
			string file_info = fileInfoMap[msgType];
			vector<string> ss = split(file_info, ' ');
			stringstream sstr;
			sstr << fileSaveDir << ss[1] << "/";
			writeFile(sstr.str(), ss[2], msg);
			fileInfoMap.erase(msgType);
		}

	}
	}
}

void SunwayMRHelper::update(HostResource hr) {
	bool updated = false;
	for (unsigned int i=0; i<allResources.size(); i++) {
		if(allResources[i].host == hr.host) {
			allResources[i].threads = hr.threads;
			allResources[i].memory = hr.memory;
			allResources[i].listenPort = hr.listenPort;
			updated = true;
			break;
		}
	}
	if(!updated) {
		allResources.push_back(hr);
	}

	saveLocalHostFile();
	saveAllHostsFile();
}

void SunwayMRHelper::saveLocalHostFile() {
	stringstream ss;
	ss << localAddr << " " << threads << " " << memory << " " << listenPort;
	writeFile(fileSaveDir, localHostFileName, ss.str());
}

void SunwayMRHelper::saveAllHostsFile() {
	stringstream ss;
	for(unsigned int i=0; i<allResources.size(); i++) {
		ss << allResources[i].host << " " << allResources[i].threads << " " << allResources[i].memory << " " << allResources[i].listenPort << endl;
	}
	writeFile(fileSaveDir, allHostsFileName, ss.str());
}


long SunwayMRHelper::getCurrentTime()
{
   struct timeval tv;
   gettimeofday(&tv,NULL);
   return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int SunwayMRHelper::randomValue(int start, int end) {
	int ret = start;
	srand(getCurrentTime());
	if (start != end) {
		if (start < end) {
			ret = start + rand() % (end - start);
		} else {
			ret = end + rand() % (start - end);
		}
	}
	return ret;
}

string SunwayMRHelper::getLocalHost() {
	string ret = "";

    struct ifaddrs * ifAddrStruct=NULL;
    struct ifaddrs * ifa=NULL;
    void * tmpAddrPtr=NULL;

    getifaddrs(&ifAddrStruct);

    for (ifa = ifAddrStruct; ifa != NULL; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family == AF_INET) { // check it is IP4
            // is a valid IP4 Address
            tmpAddrPtr=&((struct sockaddr_in *)ifa->ifa_addr)->sin_addr;
            char addressBuffer[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, tmpAddrPtr, addressBuffer, INET_ADDRSTRLEN);
            //printf("%s IP Address %s\n", ifa->ifa_name, addressBuffer);

            string temp = string(addressBuffer);
            string lo = "127.0.0.1";
            if (!temp.compare(0, lo.size(), lo)) ret = temp;

        } else if (ifa->ifa_addr->sa_family == AF_INET6) { // check it is IP6
            // is a valid IP6 Address
            tmpAddrPtr=&((struct sockaddr_in6 *)ifa->ifa_addr)->sin6_addr;
            char addressBuffer[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, tmpAddrPtr, addressBuffer, INET6_ADDRSTRLEN);
            //printf("%s IP Address %s\n", ifa->ifa_name, addressBuffer);
        }
    }
    if (ifAddrStruct!=NULL) freeifaddrs(ifAddrStruct);

    return ret;
}

void SunwayMRHelper::mkdirRecursive(const char *dir) {
        char tmp[256];
        char *p = NULL;
        size_t len;

        snprintf(tmp, sizeof(tmp),"%s",dir);
        len = strlen(tmp);
        if(tmp[len - 1] == '/')
                tmp[len - 1] = 0;
        for(p = tmp + 1; *p; p++)
                if(*p == '/') {
                        *p = 0;
                        mkdir(tmp, S_IRWXU);
                        *p = '/';
                }
        mkdir(tmp, S_IRWXU);
}


bool SunwayMRHelper::writeFile(string dir, string fileName, string content) {
	mkdirRecursive(dir.c_str());
	stringstream path;
	path << dir << fileName;
	ofstream file;
	file.open(path.str().c_str(), ios::out | ios::trunc);
	if (file.is_open()) {
		file << content;
		file.close();
		return true;
	} else {
		stringstream ss;
		ss << "unable to write file: " << path;
		logError(ss.str());
		return false;
	}
}

bool SunwayMRHelper::readFile(string path,  string &content) {
	std::ifstream file(path.c_str(), std::ifstream::in);
	if (file.is_open()) {
		std::stringstream buffer;
		buffer << file.rdbuf();
		content = buffer.str();
		file.close();
		return true;
	} else {
		stringstream ss;
		ss << "unable to write file: " << path;
		logError(ss.str());
		return false;
	}
}

int main(int argc, char *argv[]) {

	cout<< "hello helper" << endl;
	return 0;
}

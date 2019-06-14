
#include "pch.h"
#include "framework.h"
#include "ProxyServerC++.h"

#include<iostream>
#include<fstream>
#include<process.h>
#include<afxsock.h>
#include<sstream>
#include<vector>
#include <mutex>
#include <ctime>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif


// The one and only application object

CWinApp theApp;

using namespace std;

const UINT PORT = 8888u;
const time_t MAX_AGE = 86400;			// Max-age for cache file is 24h

const string FILE_BLACKLIST = "blacklist.conf";
const string FOLDER_CACHE = "cache";
vector<string> BlackList;

mutex _mutex;

typedef struct _HttpRequest {
	string Method = "";
	string Request_URI = "";
	string Version = "";
	string Host = "";
	string Port = "80";			//default port is "80"
}HttpRequest;

BOOL isForbidden = FALSE;
string html403error_file = "";

//Ref: http://stackoverflow.com/questions/19715144/how-to-convert-char-to-lpcwstr
wchar_t* convertCharArrayToLPCWSTR(const char* charArray)
{
	wchar_t* wString = new wchar_t[4096];
	MultiByteToWideChar(CP_ACP, 0, charArray, -1, wString, 4096);
	return wString;
}

char* get_ip(const char* host)
{
	struct hostent* hent;
	struct in_addr** addr_list;

	//memset(hent, 0, sizeof(hent));
	int iplen = 15; //XXX.XXX.XXX.XXX
	char* ip = (char*)malloc(iplen + 1);
	memset(ip, 0, iplen + 1);
	if ((hent = gethostbyname(host)) == NULL)
	{
		printf("Can't get IP\n");
		return NULL;
	}
	/*if (inet_ntop(AF_INET, (void *)hent->h_addr_list[0], ip, iplen) == NULL)
	{
	printf("Can't resolve host %s\n", host);
	return NULL;
	}*/
	addr_list = (struct in_addr**)hent->h_addr_list;
	if (addr_list[0] == NULL)
		return NULL;
	strcpy(ip, inet_ntoa(*addr_list[0]));
	return ip;
}

//https://www.w3.org/Protocols/rfc2616/rfc2616-sec5.html
// This function get method, uri, http version and host name from 
// request string, then set them for struct HttpRequest
void requestParse(const string request, HttpRequest &result)
{
	string method = "", uri = "", httpver = "", host = "";

	int i;
	//Get method
	for (i = 0;; i++)
	{
		if (request[i] == ' ')
			break;
		method.push_back(request[i]);
	}
	result.Method = method;

	//Get URI
	i++;
	for (;; i++)
	{
		if (request[i] == ' ')
			break;
		uri.push_back(request[i]);
	}
	result.Request_URI = uri;

	//Get http version
	i++;
	for (;; i++)
	{
		if (request[i] == '\r' || request[i] == '\n')
			break;
		httpver.push_back(request[i]);
	}
	result.Version = httpver;

	//Get host name
	i = request.find("Host"); //Host: www.abc.edu
	for (i += 6;; i++)
	{
		if (request[i] == '\r' || request[i] == '\n')
			break;
		host.push_back(request[i]);
	}
	result.Host = host;
}

// To go to the cache file, we need format Uri, avoid '/' character
// Example: ../cache/abc.com.vn/info/	(no folder with path: cache/abc.com.vn)

// This function reformat URI: convert '/' and ':' character to '-' character
string ReFormatUri(const string &uri)
{
	char c;
	string result = "";
	int i = 0;	
	for (i += 7; i < uri.length(); i++)
	{
		c = uri.at(i);
		if (c == '/' || c == ':')			//convert '/' and ':' to '-'
			c = '-';
		result.push_back(c);
	}
	return result;
}

DWORD WINAPI Proxy_func(LPVOID Param)
{
	SOCKET* hConnected = (SOCKET*)Param;
	CSocket client;
	client.Attach(*hConnected);

	int len = 100, nBytes_receive = 0;
	char* buffer = new char[len];
	string request, response;
	while (1) {
		request = "";
		response = "";

		do {
			//Waiting to receive request from client
			nBytes_receive = client.Receive(buffer, len, 0); //Blocking
			if (nBytes_receive > 0)
				request.append(buffer, nBytes_receive);

		} while (nBytes_receive == len);	//Loop until receive all datas 
		request += "\0";
		HttpRequest* httpHeader = new HttpRequest;
		if (request.length() <= 0) 
			break;

		cout << request << endl;
		requestParse(request, *httpHeader);

		//Check blacklist
		for (int i = 0; i < BlackList.size(); i++)
		{
			if (httpHeader->Host == BlackList.at(i))
			{
				isForbidden = TRUE;
				break;
			}
		}

		
		//Send response if fobidden flag is true
		if (isForbidden) {
			client.Send(html403error_file.c_str(), html403error_file.length());
			break;
		}

		//Check supported method:
		if (httpHeader->Method != "GET" && httpHeader->Method != "POST")
		{
			response = httpHeader->Host;
			response += " 501 Not Implemented\r\n";
			client.Send(response.c_str(), response.length());
			break;
		}

		time_t now = time(0);				//number seconds from 1/1/1970 to now

		// File cache in cache folder
		string file = FOLDER_CACHE + '/' + ReFormatUri(httpHeader->Request_URI);

		ifstream time_file = ifstream(file + "-time", ios_base::binary);

		if (time_file.is_open()) 
		{
			time_t time = 0;
			time_file >> time;
			if (now - time < MAX_AGE) 
			{
				ifstream data_file = ifstream(file + "-data", ios_base::binary);
				if (data_file.is_open())
				{
					while (!data_file.eof()) {
						data_file.read(buffer, len);			//read data from file, then send to client
						client.Send(buffer, len);
					}
					time_file.close();
					data_file.close();
					continue;		//Redirect 
				}
			}
		}

		//Create socket and send request to server
		CSocket web;
		web.Create(0, SOCK_STREAM);
		if (web.Connect(convertCharArrayToLPCWSTR(get_ip(httpHeader->Host.c_str())), atoi(httpHeader->Port.c_str())) < 0)
		{
			cout << "Can't Connect to Web" << endl;
			break;
		}
		else
		{
			ofstream time_file = ofstream(file + "-time", ios_base::binary);
			time_file << now;
			//Send request to server
			web.Send(request.c_str(), request.length());

			//receive data from server
			ofstream data_file = ofstream(file + "-data", ios_base::binary);
			do {

				nBytes_receive = web.Receive(buffer, len);

				//Write cache
				data_file.write(buffer, nBytes_receive);

				//send response to client
				client.Send(buffer, nBytes_receive);
			} while (nBytes_receive > 0);

			time_file.close();
			data_file.close();
			web.Close();
		}
	}

	client.Close();
	delete[] buffer;
	ExitThread(0);
}


int main()
{
	int nRetCode = 0;

	HMODULE hModule = ::GetModuleHandle(nullptr);

	if (hModule != nullptr)
	{
		// initialize MFC and print and error on failure
		if (!AfxWinInit(hModule, nullptr, ::GetCommandLine(), 0))
		{
			// TODO: code your application's behavior here.
			wprintf(L"Fatal Error: MFC initialization failed\n");
			nRetCode = 1;
		}
		else
		{
			cout << "Starting Proxy Server..." << endl;

			//Load blacklist
			cout << "Loading black list..." << endl;

			ifstream readstream(FILE_BLACKLIST);
			string temp;
			if (readstream.is_open()) {
				while (!readstream.eof()) {
					getline(readstream, temp);
					if (temp == "")
						continue;
					BlackList.push_back(temp);
				}
				cout << "Loaded" << endl;
			}
			else
				cout << "File not found!" << endl;

			//Load file 403-error response
			string line;
			ifstream readhtml("403-forbidden-error.html");
			if (readhtml.is_open())
			{
				while (!readhtml.eof())
				{
					getline(readhtml, line);
					line += "\r\n";
					html403error_file.append(line);
				}
			}

			AfxSocketInit();
			CSocket ProxyListen;
			int iResult;
			iResult = ProxyListen.Create(PORT, SOCK_STREAM);
			if (iResult < 0)
			{
				cout << "Create failed with error: " << GetLastError() << endl;
				return nRetCode;
			}
			iResult = ProxyListen.Bind(PORT);
			if (iResult < 0)
			{
				cout << "Bind failed with error: " << GetLastError() << endl;
				return nRetCode;
			}
			iResult = ProxyListen.Listen();
			if (iResult < 0)
			{
				cout << "Listen failed with error: " << GetLastError() << endl;
				return nRetCode;
			}

			else
			{
				while (1) {
					DWORD dwThreadId;
					HANDLE hThread;
					CSocket sConnected;
					if (ProxyListen.Accept(sConnected) == FALSE) {
						cout << "Cannot accept with error code: " << GetLastError();
						return nRetCode;
					}
					cout << "Connect is successful!" << endl;

					//CSocket mutilthread process:
					//Detach the socket from the CSocket instance and pass the socket to the other 
					//thread and then attach the socket to a new CSocket instance in the other 
					//thread. 

					// Detach the newly accepted socket and save the SOCKET handle
					SOCKET* hConnected = new SOCKET;
					*hConnected = sConnected.Detach();

					//Create Thread
					_mutex.lock();
					hThread = CreateThread(NULL, 0, Proxy_func, hConnected, 0, &dwThreadId);
					_mutex.unlock();
					CloseHandle(hThread);
					isForbidden = FALSE;
				}
			}
		}
	}
	else
	{
		// TODO: change error code to suit your needs
		wprintf(L"Fatal Error: GetModuleHandle failed\n");
		nRetCode = 1;
	}
	return nRetCode;
}

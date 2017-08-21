#include "GT_Select_Core.h"
#include "GTUtlity/GT_Util_GlogWrapper.h"
#include "GTUtlity/GT_Util_CfgHelper.h"
#include "GT_Select_Resource_Manager.h"

#include <algorithm>

namespace GT {
    namespace NET {

#ifndef GT_SELECT_RESOURCE_MANAGER
#define GT_SELECT_RESOURCE_MANAGER GT_Select_Resource_Manager::GetInstance()
#endif

        GT_Select_Core::GT_Select_Core():end_thread_(false)
        {
			for (auto& i : socket_set_pos_) {
				i = 0;
			}
            udp_port_ = -1;
			select_cb_func_ = NULL;
			service_started_ = false;
			service_inited_ = false;
        }


        GT_Select_Core::~GT_Select_Core()
        {
        }


		bool GT_Select_Core::Initialize() {
			if (service_inited_)
				return true;

			do 
			{
				/* reset fd set */
				for (auto& iter : socketset) {
					FD_ZERO((fd_set*)&iter);
				}

				/* init socket environment */
				int err;
				WORD	version = MAKEWORD(2, 2);
				WSADATA wsadata;
				err = WSAStartup(version, &wsadata);
				if (err != 0) {
					GT_LOG_ERROR("WSAStartup failed, error code = " << WSAGetLastError());
					WSACleanup();
					break;
				}
				if (LOBYTE(wsadata.wVersion) != 2 || (HIBYTE(wsadata.wVersion) != 2)) {
					GT_LOG_ERROR("no property winsock version can be use!");
					break;
				}

				/* create listen socket */
				listen_socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

				/* bind local address */
				SOCKADDR_IN server_sock_addr;
				memset(&server_sock_addr, 0, sizeof(SOCKADDR_IN));
				server_sock_addr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
				server_sock_addr.sin_family =  AF_INET;
				server_sock_addr.sin_port = htons(GT_READ_CFG_INT("server_cfg", "server_port", 2020));
				err = bind(listen_socket_, (sockaddr*)&server_sock_addr, sizeof(SOCKADDR_IN));
				if (!err) {
					GT_LOG_ERROR("bind socket to local failed, error code = " << WSAGetLastError());
					break;
				}

				/* listen on the socket */
				err = listen(listen_socket_, SOMAXCONN);
				if (!err) {
					GT_LOG_ERROR("listen socket failed, error code = " << WSAGetLastError());
					break;
				}

				/* add listen socket to fd read sockets set */
				AddEvent_(EVENT_ACCEPT, listen_socket_);

				service_inited_ = true;

			} while (0);

			return service_inited_;
		}

		void GT_Select_Core::StartGTService() {
			GT_TRACE_FUNCTION;
			GT_LOG_INFO("GT Select Service Start!");
			if (service_started_)
				return;
			server_thread_ = std::thread(&GT_Select_Core::Select_service_, this);
			service_started_ = true;
		}

		void GT_Select_Core::Select_service_() {
			GT_TRACE_FUNCTION;
			while (!end_thread_) {
				fd_set_pri& readset = socketset[0];
				fd_set_pri& writeset = socketset[1];
				fd_set_pri& expset = socketset[2];

				int fd_count = readset.sock_count > writeset.sock_count ? readset.sock_count > expset.sock_count ? readset.sock_count : expset.sock_count : writeset.sock_count;
				if (!fd_count) {
					GT_LOG_ERROR("Select Service Got No Socket to Serve, Just Break Out!");
					end_thread_ = true;
					return;
				}

				int ret = select(NULL, (fd_set*)&readset, (fd_set*)&writeset, (fd_set*)&expset, NULL);

				if (ret == SOCKET_ERROR) {
					GT_LOG_ERROR("got error from select, error code = " << WSAGetLastError());
					continue;
				}
				else if (!ret) {
					GT_LOG_DEBUG("select returned may got timeout!");
					continue;
				}

				for (auto& iter : readset.fd_sock_array) {
					if (FD_ISSET(iter, (fd_set*)&readset)) {
						ProcessReadEvent_(iter);
					}
				}

				for (auto& iter : writeset.fd_sock_array) {
					if (FD_ISSET(iter, (fd_set*)&writeset)) {
						ProcessWriteEvent_(iter);
					}
				}

				for (auto& iter : expset.fd_sock_array) {
					if (FD_ISSET(iter, (fd_set*)&expset)) {
						ProcessExpEvent_(iter);
					}
				}
			}
			GT_LOG_DEBUG("service thread exit!");
		}

		void GT_Select_Core::ProcessAcceptEvent_() {
			GT_TRACE_FUNCTION;
			SOCKADDR_IN client_addr;
			int size_ = sizeof(client_addr);
			SOCKET s = accept(listen_socket_, (SOCKADDR*)&client_addr, &size_);
			AddEvent_(EVENT_ACCEPT, s);
		}

		void GT_Select_Core::ProcessExpEvent_(SOCKET& s) {
			GT_TRACE_FUNCTION;
			GT_LOG_DEBUG("Got exception event!");
		}

		void GT_Select_Core::ProcessReadEvent_(SOCKET& s) {
			if (s == listen_socket_){
				ProcessAcceptEvent_();
			}
			else {
				select_buffer* bu = GT_SELECT_RESOURCE_MANAGER.GetSelectBuffer();
                SOCKADDR_IN client_addr;
                int size_ = sizeof(SOCKADDR_IN);
                getpeername(s, (SOCKADDR*)&client_addr, &size_);
                int ret = recv(s, bu->data, bu->buffer_len, NULL);

                if (client_addr.sin_port == udp_port_) {
                    GT_LOG_DEBUG("get exit flag, service will exit!");
                    return;
                }

				if (!ret) {
                    DelEvent_(EVENT_READ, s);
                    GT_LOG_DEBUG("client exit, client ip addr = " << inet_ntoa(client_addr.sin_addr) << ", port = " << client_addr.sin_port);
				}
				else if(ret == SOCKET_ERROR){
					GT_LOG_ERROR("recv got error, error code = " << WSAGetLastError());
				}
				else {
					DispatchEvent_(EVENT_READ, (PULONG_PTR)&s, bu->data, ret);
				}
			}

		}

		void GT_Select_Core::ProcessWriteEvent_(SOCKET& s) {
			GT_TRACE_FUNCTION;
			GT_LOG_DEBUG("Got write event!");
		}

		void GT_Select_Core::AddEvent_(EVENT_TYPE type, SOCKET s) {
		
			if (socket_set_pos_[type] == socketset[type].sock_count) {	/* socket pos record the next used socket position */
				GrowSet_(type);
			}
			socketset[type].fd_sock_array[socket_set_pos_[type]] = s;
			socket_set_pos_[type] ++;
		}

		void GT_Select_Core::GrowSet_(EVENT_TYPE type) {
			GT_TRACE_FUNCTION;
			int grow_size = GT_READ_CFG_INT("select_control", "fd_grow_size", 100);
			SOCKET* set_pos = socketset[type].fd_sock_array + socket_set_pos_[type];
			set_pos = new SOCKET[grow_size];
			socketset[type].sock_count += grow_size;
		}

		void GT_Select_Core::DelEvent_(EVENT_TYPE type, SOCKET s) {
			GT_TRACE_FUNCTION;
			GT_LOG_DEBUG("Collect Socket Resource...");
			int type_t = 0;
			for (auto& ss : socketset) {
				for (auto& iter : ss.fd_sock_array) {
					if (iter == s) {
						delete &iter;
						iter = ss.fd_sock_array[--ss.sock_count];/* move the end socket behind the del index to the index of the del */
						socket_set_pos_[type_t]--;
						break;
					}
				}
				type_t++;
			}
		}

		void GT_Select_Core::RegisterCallback(internal_call_back cb) {
			select_cb_func_ = cb;
		}

		void GT_Select_Core::UnRegisterCallback() {
			select_cb_func_ = NULL;
		}

		void GT_Select_Core::DispatchEvent_(EVENT_TYPE type, PULONG_PTR sock_ptr, char* data, size_t len) {
			if (!select_cb_func_)
				select_cb_func_(type, sock_ptr, data, len);
		}

		void GT_Select_Core::StopService_() {
			GT_TRACE_FUNCTION;
			if (!end_thread_) {
				end_thread_ = true;

                WakeupSelectThread_();  /* wake up select before join on the server thread */

				if (server_thread_.joinable()) {
					server_thread_.join();
				}
				GT_LOG_DEBUG("select server service thread exited!");
			}
		}


        void GT_Select_Core::WakeupSelectThread_() {
            GT_TRACE_FUNCTION;
            GT_LOG_INFO("trying wakeup select thread...");
            
            /* use UDP connect to itself to wakeup select within timeout */
                        
            SOCKET udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);/* create a UDP socket */
            
            sockaddr_in udp_addr;
            udp_addr.sin_addr.S_un.S_addr = htonl(INADDR_LOOPBACK);
            udp_addr.sin_port = htons(0);
            udp_addr.sin_family = AF_INET;
            bind(udp_sock, (sockaddr*)&udp_addr, sizeof(sockaddr_in));/* bind the socket */

            sockaddr_in temp_addr;
            int addr_len = sizeof(temp_addr);
            getsockname(udp_sock, (sockaddr*)&temp_addr, &addr_len);
            udp_port_ = temp_addr.sin_port;

            AddEvent_(EVENT_READ, udp_sock);/* add the socket to select fd set */
           
            connect(udp_sock, (sockaddr*)&udp_addr, sizeof(sockaddr_in)); /* connect UDP socket to itself, this will wakeup the select */
        }

        
		void GT_Select_Core::CollectResource_() {
			GT_TRACE_FUNCTION;
			for (auto& iter : socketset) {
				delete[] socketset->fd_sock_array;
			}
			
		}

		bool GT_Select_Core::Finalize() {
			GT_TRACE_FUNCTION;
			if (service_started_) {
				StopService_();
				CollectResource_();
			}
            return true;
		}

    }
}
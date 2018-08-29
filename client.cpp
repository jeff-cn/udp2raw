#include "common.h"
#include "network.h"
#include "connection.h"
#include "misc.h"
#include "log.h"
#include "lib/md5.h"
#include "encrypt.h"
#include "fd_manager.h"


int client_on_timer(conn_info_t &conn_info) //for client. called when a timer is ready in epoll
{
	//keep_iptables_rule();
	packet_info_t &send_info=conn_info.raw_info.send_info;
	packet_info_t &recv_info=conn_info.raw_info.recv_info;
	raw_info_t &raw_info=conn_info.raw_info;
	conn_info.blob->conv_manager.c.clear_inactive();
	mylog(log_trace,"timer!\n");

	mylog(log_trace,"roller my %d,oppsite %d,%lld\n",int(conn_info.my_roller),int(conn_info.oppsite_roller),conn_info.last_oppsite_roller_time);

	mylog(log_trace,"<client_on_timer,send_info.ts_ack= %u>\n",send_info.ts_ack);

	if(raw_info.disabled)
	{
		conn_info.state.client_current_state=client_idle;
		conn_info.my_id=get_true_random_number_nz();

		mylog(log_info,"state back to client_idle\n");
	}

	if(conn_info.state.client_current_state==client_idle)
	{
		raw_info.rst_received=0;
		raw_info.disabled=0;

		fail_time_counter++;
		if(max_fail_time>0&&fail_time_counter>max_fail_time)
		{
			mylog(log_fatal,"max_fail_time exceed\n");
			myexit(-1);
		}

		conn_info.blob->anti_replay.re_init();
		conn_info.my_id = get_true_random_number_nz(); ///todo no need to do this everytime



		address_t tmp_addr;
		//u32_t new_ip=0;
		if(!force_source_ip)
		{
			if(get_src_adress2(tmp_addr,remote_addr)!=0)
			{
				mylog(log_warn,"get_src_adress() failed\n");
				return -1;
			}
			//source_addr=new_addr;
			//source_addr.set_port(0);

			mylog(log_info,"source_addr is now %s\n",tmp_addr.get_ip());

			/*
			if(new_ip!=source_ip_uint32)
			{
				mylog(log_info,"source ip changed from %s to ",my_ntoa(source_ip_uint32));
				log_bare(log_info,"%s\n",my_ntoa(new_ip));
				source_ip_uint32=new_ip;
				send_info.src_ip=new_ip;
			}*/

		}
		else
		{
			tmp_addr=source_addr;
		}

		send_info.new_src_ip.from_address_t(tmp_addr);

		if (force_source_port == 0)
		{
			send_info.src_port = client_bind_to_a_new_port2(bind_fd,tmp_addr);
		}
		else
		{
			send_info.src_port = source_port;
		}

		if (raw_mode == mode_icmp)
		{
			send_info.dst_port = send_info.src_port;
		}

		mylog(log_info, "using port %d\n", send_info.src_port);
		init_filter(send_info.src_port);

		if(raw_mode==mode_icmp||raw_mode==mode_udp)
		{
			conn_info.state.client_current_state=client_handshake1;

			mylog(log_info,"state changed from client_idle to client_pre_handshake\n");
		}
		if(raw_mode==mode_faketcp)
		{
			conn_info.state.client_current_state=client_tcp_handshake;
			mylog(log_info,"state changed from client_idle to client_tcp_handshake\n");

		}
		conn_info.last_state_time=get_current_time();
		conn_info.last_hb_sent_time=0;
		//dont return;
	}
	if(conn_info.state.client_current_state==client_tcp_handshake)  //send and resend syn
	{
		assert(raw_mode==mode_faketcp);
		if (get_current_time() - conn_info.last_state_time > client_handshake_timeout)
		{
			conn_info.state.client_current_state = client_idle;
			mylog(log_info, "state back to client_idle from client_tcp_handshake\n");
			return 0;

		}
		else if (get_current_time() - conn_info.last_hb_sent_time > client_retry_interval)
		{

			if (raw_mode == mode_faketcp)
			{
				if (conn_info.last_hb_sent_time == 0)
				{
					send_info.psh = 0;
					send_info.syn = 1;
					send_info.ack = 0;
					send_info.ts_ack =0;
					send_info.seq=get_true_random_number();
					send_info.ack_seq=get_true_random_number();
				}
			}

			send_raw0(raw_info, 0, 0);

			conn_info.last_hb_sent_time = get_current_time();
			mylog(log_info, "(re)sent tcp syn\n");
			return 0;
		}
		else
		{
			return 0;
		}
		return 0;
	}
	else if(conn_info.state.client_current_state==client_handshake1)//send and resend handshake1
	{
		if(get_current_time()-conn_info.last_state_time>client_handshake_timeout)
		{
			conn_info.state.client_current_state=client_idle;
			mylog(log_info,"state back to client_idle from client_handshake1\n");
			return 0;

		}
		else if(get_current_time()-conn_info.last_hb_sent_time>client_retry_interval)
		{

			if(raw_mode==mode_faketcp)
			{
				if(conn_info.last_hb_sent_time==0)
				{
					send_info.seq++;
					send_info.ack_seq=recv_info.seq+1;
					send_info.ts_ack=recv_info.ts;
					raw_info.reserved_send_seq=send_info.seq;
				}
				send_info.seq=raw_info.reserved_send_seq;
				send_info.psh = 0;
				send_info.syn = 0;
				send_info.ack = 1;
				send_raw0(raw_info, 0, 0);

				send_handshake(raw_info,conn_info.my_id,0,const_id);

				send_info.seq+=raw_info.send_info.data_len;
			}
			else
			{

				send_handshake(raw_info,conn_info.my_id,0,const_id);
				if(raw_mode==mode_icmp)
					send_info.my_icmp_seq++;
			}

			conn_info.last_hb_sent_time=get_current_time();
			mylog(log_info,"(re)sent handshake1\n");
			return 0;
		}
		else
		{
			return 0;
		}
		return 0;
	}
	else if(conn_info.state.client_current_state==client_handshake2)
	{
		if(get_current_time()-conn_info.last_state_time>client_handshake_timeout)
		{
			conn_info.state.client_current_state=client_idle;
			mylog(log_info,"state back to client_idle from client_handshake2\n");
			return 0;
		}
		else if(get_current_time()-conn_info.last_hb_sent_time>client_retry_interval)
		{
			if(raw_mode==mode_faketcp)
			{
				if(conn_info.last_hb_sent_time==0)
				{
					send_info.ack_seq=recv_info.seq+raw_info.recv_info.data_len;
					send_info.ts_ack=recv_info.ts;
					raw_info.reserved_send_seq=send_info.seq;
				}
				send_info.seq=raw_info.reserved_send_seq;
				send_handshake(raw_info,conn_info.my_id,conn_info.oppsite_id,const_id);
				send_info.seq+=raw_info.send_info.data_len;

			}
			else
			{

				send_handshake(raw_info,conn_info.my_id,conn_info.oppsite_id,const_id);
				if(raw_mode==mode_icmp)
					send_info.my_icmp_seq++;
			}
			conn_info.last_hb_sent_time=get_current_time();
			mylog(log_info,"(re)sent handshake2\n");
			return 0;

		}
		else
		{
			return 0;
		}
		return 0;
	}
	else if(conn_info.state.client_current_state==client_ready)
	{
		fail_time_counter=0;
		mylog(log_trace,"time %llu,%llu\n",get_current_time(),conn_info.last_state_time);

		if(get_current_time()-conn_info.last_hb_recv_time>client_conn_timeout)
		{
			conn_info.state.client_current_state=client_idle;
			conn_info.my_id=get_true_random_number_nz();
			mylog(log_info,"state back to client_idle from  client_ready bc of server-->client direction timeout\n");
			return 0;
		}

		if(get_current_time()- conn_info.last_oppsite_roller_time>client_conn_uplink_timeout)
		{
			conn_info.state.client_current_state=client_idle;
			conn_info.my_id=get_true_random_number_nz();
			mylog(log_info,"state back to client_idle from  client_ready bc of client-->server direction timeout\n");
		}


		if(get_current_time()-conn_info.last_hb_sent_time<heartbeat_interval)
		{
			return 0;
		}



		mylog(log_debug,"heartbeat sent <%x,%x>\n",conn_info.oppsite_id,conn_info.my_id);

		if(hb_mode==0)
			send_safer(conn_info,'h',hb_buf,0);/////////////send
		else
			send_safer(conn_info,'h',hb_buf,hb_len);
		conn_info.last_hb_sent_time=get_current_time();
		return 0;
	}
	else
	{
		mylog(log_fatal,"unknown state,this shouldnt happen.\n");
		myexit(-1);
	}
	return 0;
}
int client_on_raw_recv(conn_info_t &conn_info) //called when raw fd received a packet.
{
	char* data;int data_len;
	packet_info_t &send_info=conn_info.raw_info.send_info;
	packet_info_t &recv_info=conn_info.raw_info.recv_info;

	raw_info_t &raw_info=conn_info.raw_info;

	mylog(log_trace,"<client_on_raw_recv,send_info.ts_ack= %u>\n",send_info.ts_ack);
	if(pre_recv_raw_packet()<0) return -1;

	if(conn_info.state.client_current_state==client_idle )
	{

		discard_raw_packet();
		//recv(raw_recv_fd, 0,0, 0  );
	}
	else if(conn_info.state.client_current_state==client_tcp_handshake)//received syn ack
	{
		assert(raw_mode==mode_faketcp);
		if(recv_raw0(raw_info,data,data_len)<0)
		{
			return -1;
		}
		if(!recv_info.new_src_ip.equal(send_info.new_dst_ip)||recv_info.src_port!=send_info.dst_port)
		{
			mylog(log_debug,"unexpected adress %s %s %d %d\n",recv_info.new_src_ip.get_str1(),send_info.new_dst_ip.get_str2(),recv_info.src_port,send_info.dst_port);
			return -1;
		}
		if(data_len==0&&raw_info.recv_info.syn==1&&raw_info.recv_info.ack==1)
		{
			if(recv_info.ack_seq!=send_info.seq+1)
			{
				mylog(log_debug,"seq ack_seq mis match\n");
							return -1;
			}

			conn_info.state.client_current_state = client_handshake1;
			mylog(log_info,"state changed from client_tcp_handshake to client_handshake1\n");
			conn_info.last_state_time = get_current_time();
			conn_info.last_hb_sent_time=0;
			client_on_timer(conn_info);
			return 0;
		}
		else
		{
			mylog(log_debug,"unexpected packet type,expected:syn ack\n");
			return -1;
		}
	}
	else if(conn_info.state.client_current_state==client_handshake1)//recevied respond of handshake1
	{
		if(recv_bare(raw_info,data,data_len)!=0)
		{
			mylog(log_debug,"recv_bare failed!\n");
			return -1;
		}
		if(!recv_info.new_src_ip.equal(send_info.new_dst_ip)||recv_info.src_port!=send_info.dst_port)
		{
			mylog(log_debug,"unexpected adress %s %s %d %d\n",recv_info.new_src_ip.get_str1(),send_info.new_dst_ip.get_str2(),recv_info.src_port,send_info.dst_port);
			return -1;
		}
		if(data_len<int( 3*sizeof(my_id_t)))
		{
			mylog(log_debug,"too short to be a handshake\n");
			return -1;
		}
		//id_t tmp_oppsite_id=  ntohl(* ((u32_t *)&data[0]));
		my_id_t tmp_oppsite_id;
		memcpy(&tmp_oppsite_id,&data[0],sizeof(tmp_oppsite_id));
		tmp_oppsite_id=ntohl(tmp_oppsite_id);

		//id_t tmp_my_id=ntohl(* ((u32_t *)&data[sizeof(id_t)]));
		my_id_t tmp_my_id;
		memcpy(&tmp_my_id,&data[sizeof(my_id_t)],sizeof(tmp_my_id));
		tmp_my_id=ntohl(tmp_my_id);

		//id_t tmp_oppsite_const_id=ntohl(* ((u32_t *)&data[sizeof(id_t)*2]));
		my_id_t tmp_oppsite_const_id;
		memcpy(&tmp_oppsite_const_id,&data[sizeof(my_id_t)*2],sizeof(tmp_oppsite_const_id));
		tmp_oppsite_const_id=ntohl(tmp_oppsite_const_id);

		if(tmp_my_id!=conn_info.my_id)
		{
			mylog(log_debug,"tmp_my_id doesnt match\n");
			return -1;
		}


		if(raw_mode==mode_faketcp)
		{
			if(recv_info.ack_seq!=send_info.seq)
			{
				mylog(log_debug,"seq ack_seq mis match\n");
							return -1;
			}
			if(recv_info.seq!=send_info.ack_seq)
			{
				mylog(log_debug,"seq ack_seq mis match\n");
							return -1;
			}
		}
		conn_info.oppsite_id=tmp_oppsite_id;

		mylog(log_info,"changed state from to client_handshake1 to client_handshake2,my_id is %x,oppsite id is %x\n",conn_info.my_id,conn_info.oppsite_id);

		//send_handshake(raw_info,conn_info.my_id,conn_info.oppsite_id,const_id);  //////////////send
		conn_info.state.client_current_state = client_handshake2;
		conn_info.last_state_time = get_current_time();
		conn_info.last_hb_sent_time=0;
		client_on_timer(conn_info);

		return 0;
	}
	else if(conn_info.state.client_current_state==client_handshake2||conn_info.state.client_current_state==client_ready)//received heartbeat or data
	{
		char type;
		if(recv_safer(conn_info,type,data,data_len)!=0)
		{
			mylog(log_debug,"recv_safer failed!\n");
			return -1;
		}
		if(!recv_info.new_src_ip.equal(send_info.new_dst_ip)||recv_info.src_port!=send_info.dst_port)
		{
			mylog(log_warn,"unexpected adress %s %s %d %d,this shouldnt happen.\n",recv_info.new_src_ip.get_str1(),send_info.new_dst_ip.get_str2(),recv_info.src_port,send_info.dst_port);
			return -1;
		}
		if(conn_info.state.client_current_state==client_handshake2)
		{
			mylog(log_info,"changed state from to client_handshake2 to client_ready\n");
			conn_info.state.client_current_state=client_ready;
			conn_info.last_hb_sent_time=0;
			conn_info.last_hb_recv_time=get_current_time();
			conn_info.last_oppsite_roller_time=conn_info.last_hb_recv_time;
			client_on_timer(conn_info);
		}
		if(data_len>=0&&type=='h')
		{
			mylog(log_debug,"[hb]heart beat received,oppsite_roller=%d\n",int(conn_info.oppsite_roller));
			conn_info.last_hb_recv_time=get_current_time();
			return 0;
		}
		else if(data_len>= int( sizeof(u32_t))&&type=='d')
		{
			mylog(log_trace,"received a data from fake tcp,len:%d\n",data_len);

			if(hb_mode==0)
				conn_info.last_hb_recv_time=get_current_time();

			//u32_t tmp_conv_id= ntohl(* ((u32_t *)&data[0]));
			u32_t tmp_conv_id;
			memcpy(&tmp_conv_id,&data[0],sizeof(tmp_conv_id));
			tmp_conv_id=ntohl(tmp_conv_id);

			if(!conn_info.blob->conv_manager.c.is_conv_used(tmp_conv_id))
			{
				mylog(log_info,"unknow conv %d,ignore\n",tmp_conv_id);
				return 0;
			}

			conn_info.blob->conv_manager.c.update_active_time(tmp_conv_id);

			//u64_t u64=conn_info.blob->conv_manager.c.find_data_by_conv(tmp_conv_id);

			address_t tmp_addr=conn_info.blob->conv_manager.c.find_data_by_conv(tmp_conv_id);

			//sockaddr_in tmp_sockaddr={0};

			//tmp_sockaddr.sin_family = AF_INET;
			//tmp_sockaddr.sin_addr.s_addr=(u64>>32u);

			//tmp_sockaddr.sin_port= htons(uint16_t((u64<<32u)>>32u));


			int ret=sendto(udp_fd,data+sizeof(u32_t),data_len -(sizeof(u32_t)),0,(struct sockaddr *)&tmp_addr.inner,tmp_addr.get_len());

			if(ret<0)
			{
		    	mylog(log_warn,"sento returned %d,%s,%02x,%s\n",ret,strerror(errno),int(tmp_addr.get_type()),tmp_addr.get_str());
				//perror("ret<0");
			}
			//mylog(log_trace,"%s :%d\n",inet_ntoa(tmp_sockaddr.sin_addr),ntohs(tmp_sockaddr.sin_port));
			mylog(log_trace,"%d byte sent\n",ret);
		}
		else
		{
			mylog(log_warn,"unknown packet,this shouldnt happen.\n");
						return -1;
		}

		return 0;
	}
	else
	{
		mylog(log_fatal,"unknown state,this shouldnt happen.\n");
		myexit(-1);
	}
	return 0;
}
int client_on_udp_recv(conn_info_t &conn_info)
{
	int recv_len;
	char buf[buf_len];
	address_t::storage_t udp_new_addr_in={0};
	socklen_t udp_new_addr_len = sizeof(address_t::storage_t);
	if ((recv_len = recvfrom(udp_fd, buf, max_data_len+1, 0,
			(struct sockaddr *) &udp_new_addr_in, &udp_new_addr_len)) == -1) {
		mylog(log_warn,"recv_from error,%s\n",strerror(errno));
		//myexit(1);
	};

	if(recv_len==max_data_len+1)
	{
		mylog(log_warn,"huge packet, data_len > %d,dropped\n",max_data_len);
		return -1;
	}

	if(recv_len>=mtu_warn)
	{
		mylog(log_warn,"huge packet,data len=%d (>=%d).strongly suggested to set a smaller mtu at upper level,to get rid of this warn\n ",recv_len,mtu_warn);
	}
	//mylog(log_trace,"Received packet from %s:%d,len: %d\n", inet_ntoa(udp_new_addr_in.sin_addr),
		//	ntohs(udp_new_addr_in.sin_port),recv_len);

	/*
	if(udp_old_addr_in.sin_addr.s_addr==0&&udp_old_addr_in.sin_port==0)
	{
		memcpy(&udp_old_addr_in,&udp_new_addr_in,sizeof(udp_new_addr_in));
	}
	else if(udp_new_addr_in.sin_addr.s_addr!=udp_old_addr_in.sin_addr.s_addr
			||udp_new_addr_in.sin_port!=udp_old_addr_in.sin_port)
	{
		if(get_current_time()- last_udp_recv_time <udp_timeout)
		{
			printf("new <ip,port> connected in,ignored,bc last connection is still active\n");
			continue;
		}
		else
		{
			printf("new <ip,port> connected in,accpeted\n");
			memcpy(&udp_old_addr_in,&udp_new_addr_in,sizeof(udp_new_addr_in));
			conv_id++;
		}
	}*/

	//last_udp_recv_time=get_current_time();
	address_t tmp_addr;
	tmp_addr.from_sockaddr((sockaddr *)&udp_new_addr_in,udp_new_addr_len);
	//u64_t u64=((u64_t(udp_new_addr_in.sin_addr.s_addr))<<32u)+ntohs(udp_new_addr_in.sin_port);
	u32_t conv;

	//u64_t u64;//////todo
	if(!conn_info.blob->conv_manager.c.is_data_used(tmp_addr))
	{
		if(conn_info.blob->conv_manager.c.get_size() >=max_conv_num)
		{
			mylog(log_warn,"ignored new udp connect bc max_conv_num exceed\n");
			return -1;
		}
		conv=conn_info.blob->conv_manager.c.get_new_conv();
		conn_info.blob->conv_manager.c.insert_conv(conv,tmp_addr);
		mylog(log_info,"new packet from %s,conv_id=%x\n",tmp_addr.get_str(),conv);
	}
	else
	{
		conv=conn_info.blob->conv_manager.c.find_conv_by_data(tmp_addr);
	}

	conn_info.blob->conv_manager.c.update_active_time(conv);

	if(conn_info.state.client_current_state==client_ready)
	{
		/*
		char buf2[6000];
		int ret1=send_raw(conn_info.raw_info,buf2,40);
		int ret2=send_raw(conn_info.raw_info,buf2,500);
		int ret3=send_raw(conn_info.raw_info,buf2,1000);
		int ret4=send_raw(conn_info.raw_info,buf2,2000);
		mylog(log_warn,"ret= %d %d %d %d\n",ret1,ret2,ret3,ret4);*/

		send_data_safer(conn_info,buf,recv_len,conv);
	}
	return 0;
}

int client_event_loop()
{


	char buf[buf_len];

	conn_info_t conn_info;
	conn_info.my_id=get_true_random_number_nz();

	conn_info.prepare();
	packet_info_t &send_info=conn_info.raw_info.send_info;
	packet_info_t &recv_info=conn_info.raw_info.recv_info;

	if(lower_level)
	{
		if(lower_level_manual)
		{
			int index;
			init_ifindex(if_name,raw_send_fd,index);
			//init_ifindex(if_name);
			memset(&send_info.addr_ll, 0, sizeof(send_info.addr_ll));
			send_info.addr_ll.sll_family = AF_PACKET;
			send_info.addr_ll.sll_ifindex =index;
			send_info.addr_ll.sll_halen = ETHER_ADDR_LEN;
			send_info.addr_ll.sll_protocol = htons(ETH_P_IP);
			memcpy(&send_info.addr_ll.sll_addr, dest_hw_addr, ETHER_ADDR_LEN);
			mylog(log_info,"we are running at lower-level (manual) mode\n");
		}
		else
		{
			u32_t dest_ip;
			string if_name_string;
			string hw_string;
			assert(remote_addr.get_type()==AF_INET);

			if(retry_on_error==0)
			{
				if(find_lower_level_info(remote_addr.inner.ipv4.sin_addr.s_addr,dest_ip,if_name_string,hw_string)!=0)
				{
					mylog(log_fatal,"auto detect lower-level info failed for %s,specific it manually\n",remote_addr.get_ip());
					myexit(-1);
				}
			}
			else
			{
				int ok=0;
				while(!ok)
				{
					if(find_lower_level_info(remote_addr.inner.ipv4.sin_addr.s_addr,dest_ip,if_name_string,hw_string)!=0)
					{
						mylog(log_warn,"auto detect lower-level info failed for %s,retry in %d seconds\n",remote_addr.get_ip(),retry_on_error_interval);
						sleep(retry_on_error_interval);
					}
					else
					{
						ok=1;
					}

				}
			}
			mylog(log_info,"we are running at lower-level (auto) mode,%s %s %s\n",my_ntoa(dest_ip),if_name_string.c_str(),hw_string.c_str());

			u32_t hw[6];
			memset(hw, 0, sizeof(hw));
			sscanf(hw_string.c_str(), "%x:%x:%x:%x:%x:%x",&hw[0], &hw[1], &hw[2],
					&hw[3], &hw[4], &hw[5]);

			mylog(log_warn,
					"make sure this is correct:   if_name=<%s>  dest_mac_adress=<%02x:%02x:%02x:%02x:%02x:%02x>  \n",
					if_name_string.c_str(), hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
			for (int i = 0; i < 6; i++) {
				dest_hw_addr[i] = uint8_t(hw[i]);
			}

			//mylog(log_fatal,"--lower-level auto for client hasnt been implemented\n");
			int index;
			init_ifindex(if_name_string.c_str(),raw_send_fd,index);

			memset(&send_info.addr_ll, 0, sizeof(send_info.addr_ll));
			send_info.addr_ll.sll_family = AF_PACKET;
			send_info.addr_ll.sll_ifindex = index;
			send_info.addr_ll.sll_halen = ETHER_ADDR_LEN;
			send_info.addr_ll.sll_protocol = htons(ETH_P_IP);
			memcpy(&send_info.addr_ll.sll_addr, dest_hw_addr, ETHER_ADDR_LEN);
			//mylog(log_info,"we are running at lower-level (manual) mode\n");
		}

	}

	/*

	address_t new_addr;

	if(!force_source_ip)
	{
		mylog(log_info,"get_src_adress called\n");
		if(retry_on_error==0)
		{
			if(get_src_adress2(new_addr,remote_addr)!=0)
			{
				mylog(log_fatal,"the trick to auto get source ip failed, maybe you dont have internet access\n");
				myexit(-1);
			}
		}
		else
		{
			int ok=0;
			while(!ok)
			{
				if(get_src_adress2(new_addr,remote_addr)!=0)
				{
					mylog(log_warn,"the trick to auto get source ip failed, maybe you dont have internet access, retry in %d seconds\n",retry_on_error_interval);
					sleep(retry_on_error_interval);
				}
				else
				{
					ok=1;
				}

			}
		}

	}
	else
	{
		new_addr=source_addr;
	}
	//in_addr tmp;
	//tmp.s_addr=source_ip_uint32;
	mylog(log_info,"source ip = %s\n",new_addr.get_ip());*/
	//printf("done\n");


	/*
	if(try_to_list_and_bind(bind_fd,local_ip_uint32,source_port)!=0)
	{
		mylog(log_fatal,"bind to source_port:%d fail\n ",source_port);
		myexit(-1);
	}*/
	send_info.src_port=0;
	memset(&send_info.new_src_ip,0,sizeof(send_info.new_src_ip));

	int i, j, k;int ret;


	//init_filter(source_port);

	send_info.new_dst_ip.from_address_t(remote_addr);

	send_info.dst_port=remote_addr.get_port();

	//g_packet_info.src_ip=source_address_uint32;
	//g_packet_info.src_port=source_port;

    udp_fd=socket(local_addr.get_type(), SOCK_DGRAM, IPPROTO_UDP);
    set_buf_size(udp_fd,socket_buf_size);

	int yes = 1;
	//setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	//struct sockaddr_in local_me={0};

	//socklen_t slen = sizeof(sockaddr_in);
	//memset(&local_me, 0, sizeof(local_me));
	//local_me.sin_family = AF_INET;
	//local_me.sin_port = local_addr.get_type();
	//local_me.sin_addr.s_addr = local_addr.inner.ipv4.sin_addr.s_addr;


	if (bind(udp_fd, (struct sockaddr*) &local_addr.inner, local_addr.get_len()) == -1) {
		mylog(log_fatal,"socket bind error\n");
		//perror("socket bind error");
		myexit(1);
	}
	setnonblocking(udp_fd);
	epollfd = epoll_create1(0);

	const int max_events = 4096;
	struct epoll_event ev, events[max_events];
	if (epollfd < 0) {
		mylog(log_fatal,"epoll return %d\n", epollfd);
		myexit(-1);
	}

	ev.events = EPOLLIN;
	ev.data.u64 = udp_fd;
	ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, udp_fd, &ev);
	if (ret!=0) {
		mylog(log_fatal,"add  udp_listen_fd error\n");
		myexit(-1);
	}
	ev.events = EPOLLIN;
	ev.data.u64 = raw_recv_fd;

	ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, raw_recv_fd, &ev);
	if (ret!= 0) {
		mylog(log_fatal,"add raw_fd error\n");
		myexit(-1);
	}

	////add_timer for fake_tcp_keep_connection_client

	//sleep(10);

	//memset(&udp_old_addr_in,0,sizeof(sockaddr_in));
	int unbind=1;

	set_timer(epollfd,timer_fd);

	mylog(log_debug,"send_raw : from %s %d  to %s %d\n",send_info.new_src_ip.get_str1(),send_info.src_port,send_info.new_dst_ip.get_str2(),send_info.dst_port);
	int fifo_fd=-1;

	if(fifo_file[0]!=0)
	{
		fifo_fd=create_fifo(fifo_file);
		ev.events = EPOLLIN;
		ev.data.u64 = fifo_fd;

		ret = epoll_ctl(epollfd, EPOLL_CTL_ADD, fifo_fd, &ev);
		if (ret!= 0) {
			mylog(log_fatal,"add fifo_fd to epoll error %s\n",strerror(errno));
			myexit(-1);
		}
		mylog(log_info,"fifo_file=%s\n",fifo_file);
	}
	while(1)////////////////////////
	{
		if(about_to_exit) myexit(0);
		epoll_trigger_counter++;
		int nfds = epoll_wait(epollfd, events, max_events, 180 * 1000);
		if (nfds < 0) {  //allow zero
			if(errno==EINTR  )
			{
				mylog(log_info,"epoll interrupted by signal,continue\n");
				//close(fifo_fd);
				//myexit(0);
			}
			else
			{
				mylog(log_fatal,"epoll_wait return %d,%s\n", nfds,strerror(errno));
				myexit(-1);
			}
		}
		int idx;
		for (idx = 0; idx < nfds; ++idx) {
			if (events[idx].data.u64 == (u64_t)raw_recv_fd)
			{
				//iphdr *iph;tcphdr *tcph;
				client_on_raw_recv(conn_info);
			}
			else if(events[idx].data.u64 ==(u64_t)timer_fd)
			{
				u64_t value;
				int unused=read(timer_fd, &value, 8);
				client_on_timer(conn_info);
				mylog(log_trace,"epoll_trigger_counter:  %d \n",epoll_trigger_counter);
				epoll_trigger_counter=0;
			}
			else if (events[idx].data.u64 == (u64_t)udp_fd)
			{
				client_on_udp_recv(conn_info);
			}
			else if (events[idx].data.u64 == (u64_t)fifo_fd)
			{
				int len=read (fifo_fd, buf, sizeof (buf));
				//assert(len>=0);
				if(len<0)
				{
					mylog(log_warn,"fifo read failed len=%d,errno=%s\n",len,strerror(errno));
					continue;
				}
				buf[len]=0;
				while(len>=1&&buf[len-1]=='\n')
					buf[len-1]=0;
				mylog(log_info,"got data from fifo,len=%d,s=[%s]\n",len,buf);
				if(strcmp(buf,"reconnect")==0)
				{
					mylog(log_info,"received command: reconnect\n");
					conn_info.state.client_current_state=client_idle;
					conn_info.my_id=get_true_random_number_nz();
				}
				else
				{
					mylog(log_info,"unknown command\n");
				}

			}
			else
			{
				mylog(log_fatal,"unknown fd,this should never happen\n");
				myexit(-1);
			}
		}
	}
	return 0;
}

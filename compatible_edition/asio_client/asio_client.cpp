
//configuration
#define SERVER_PORT		9528
#define FORCE_TO_USE_MSG_RECV_BUFFER //force to use the msg recv buffer
#define CUSTOM_LOG
#define DEFAULT_PACKER	my_packer
#define DEFAULT_UNPACKER my_unpacker

//the following three macro demonstrate how to support huge msg(exceed 65535 - 2).
//huge msg consume huge memory, for example, if we support 1M msg size, because every st_tcp_socket has a
//private unpacker which has a buffer at lest 1M size, so 1K st_tcp_socket will consume 1G memory.
//if we consider the send buffer and recv buffer, the buffer's default max size is 1K, so, every st_tcp_socket
//can consume 2G(2 * 1M * 1K) memory when performance testing(both send buffer and recv buffer are full).
//#define HUGE_MSG
//#define MAX_MSG_LEN (1024 * 1024)
//#define MAX_MSG_NUM 8 //reduce buffer size to reduce memory occupation
//configuration

//demonstrate how to use custom log system:
//notice: please don't forget to define the CUSTOM_LOG macro.
//use your own code to replace all_out_helper2 macro.
//custom log should be defined(or included) before including any st_asio_wrapper header files except
//st_asio_wrapper_base.h
#include "../include/st_asio_wrapper_base.h"
using namespace st_asio_wrapper;

class unified_out
{
public:
	static void fatal_out(const char* fmt, ...) {all_out_helper2;}
	static void error_out(const char* fmt, ...) {all_out_helper2;}
	static void warning_out(const char* fmt, ...) {all_out_helper2;}
	static void info_out(const char* fmt, ...) {all_out_helper2;}
	static void debug_out(const char* fmt, ...) {all_out_helper2;}
};

//demonstrate how to use custom msg buffer(default buffer is std::string)
#include "../include/st_asio_wrapper_packer.h"
#include "../include/st_asio_wrapper_unpacker.h"

//this buffer is more efficient than std::string if the memory is already allocated,
//because the replication been saved. for example, you are sending memory-mapped files.
class my_msg_buffer
{
public:
	my_msg_buffer() {do_detach();}
	~my_msg_buffer() {detach();}

	void assign(const char* _buff, size_t _len)
	{
		assert(_len > 0 && NULL != _buff);
		char* _buff_ = new char[_len];
		memcpy(_buff_, _buff, _len);

		attach(_buff_, _len);
	}

	void attach(const char* _buff, size_t _len) {detach(); do_attach(_buff, _len);}
	void detach() {delete buff; do_detach();}

	//the following four functions are needed by st_asio_wrapper
	//for other functions, depends on the implementation of your packer and unpacker
	bool empty() const {return 0 == len || NULL == buff;}
	const char* data() const {return buff;}
	size_t size() const {return len;}
	void swap(my_msg_buffer& other) {std::swap(buff, other.buff); std::swap(len, other.len);}

protected:
	void do_attach(const char* _buff, size_t _len) {buff = _buff; len = _len;}
	void do_detach() {buff = NULL; len = 0;}

protected:
	const char* buff;
	size_t len;
};

class my_packer : public i_packer<my_msg_buffer>
{
public:
	static size_t get_max_msg_size() {return MAX_MSG_LEN - HEAD_LEN;}
	virtual bool pack_msg(my_msg_buffer& msg, const char* const pstr[], const size_t len[], size_t num, bool native = false)
	{
		msg.detach();
		if (NULL != pstr && NULL != len)
		{
			size_t total_len = native ? 0 : HEAD_LEN;
			size_t last_total_len = total_len;
			for (size_t i = 0; i < num; ++i)
				if (NULL != pstr[i])
				{
					total_len += len[i];
					if (last_total_len > total_len || total_len > MAX_MSG_LEN) //overflow
					{
						unified_out::error_out("pack msg error: length exceeds the MAX_MSG_LEN!");
						return false;
					}
					last_total_len = total_len;
				}

				if (total_len > (native ? 0 : HEAD_LEN))
				{
					char* buff = NULL;
					size_t pos = 0;
					if (!native)
					{
						HEAD_TYPE head_len = (HEAD_TYPE) total_len;
						if (total_len != head_len)
						{
							unified_out::error_out("pack msg error: length exceeds the header's range!");
							return false;
						}

						head_len = HEAD_H2N(head_len);
						buff = new char[total_len];
						memcpy(buff, (const char*) &head_len, HEAD_LEN);
						pos = HEAD_LEN;
					}
					else
						buff = new char[total_len];

					for (size_t i = 0; i < num; ++i)
						if (NULL != pstr[i])
						{
							memcpy(buff + pos, pstr[i], len[i]);
							pos += len[i];
						}

					msg.attach(buff, total_len);
				}
		} //if (NULL != pstr && NULL != len)

		return true;
	}
};

class my_unpacker : public i_unpacker<my_msg_buffer>
{
public:
	my_unpacker() {reset_unpacker_state();}
	size_t current_msg_length() const {return raw_buff.size();} //current msg's total length(not include the head), 0 means don't know

public:
	virtual void reset_unpacker_state() {raw_buff.detach(); step = 0;}
	virtual bool parse_msg(size_t bytes_transferred, boost::container::list<my_msg_buffer>& msg_can)
	{
		if (0 == step)
		{
			assert(!raw_buff.empty());
			step = 1;
		}
		else if (1 == step)
		{
			step = 0;

			assert(!raw_buff.empty());
			if (bytes_transferred != raw_buff.size())
				return false;

			msg_can.resize(msg_can.size() + 1);
			msg_can.back().swap(raw_buff);
		}
		else
			return false;

		return true;
	}

	//a return value of 0 indicates that the read operation is complete. a non-zero value indicates the maximum number
	//of bytes to be read on the next call to the stream's async_read_some function. ---boost::asio::async_read
	virtual size_t completion_condition(const boost::system::error_code& ec, size_t bytes_transferred)
	{
		if (ec)
			return 0;

		if (0 == step) //the msg's head been received
		{
			assert(raw_buff.empty());

			if (bytes_transferred < HEAD_LEN)
				return HEAD_LEN - bytes_transferred;

			assert(HEAD_LEN == bytes_transferred);
			BOOST_AUTO(cur_msg_len, HEAD_N2H(*(HEAD_TYPE*) head_buff) - HEAD_LEN);
			if (cur_msg_len > MAX_MSG_LEN - HEAD_LEN) //invalid msg, stop reading
				step = -1;
			else
				raw_buff.attach(new char[cur_msg_len], cur_msg_len);

			return 0;
		}
		else if (1 == step)
		{
			assert(!raw_buff.empty());
			return raw_buff.size() - bytes_transferred;
		}
		else
		{
			assert(false);
			return 0;
		}
	}

	virtual boost::asio::mutable_buffers_1 prepare_next_recv() {return raw_buff.empty() ? boost::asio::buffer(head_buff) :
		boost::asio::buffer(const_cast<char*>(raw_buff.data()), raw_buff.size());}

private:
	char head_buff[HEAD_LEN]; //for head only
	my_msg_buffer raw_buff;
	int step; //-1-error format, 0-want the head, 1-want the body
};

#include "../include/st_asio_wrapper_tcp_client.h"

#define QUIT_COMMAND	"quit"
#define RESTART_COMMAND	"restart"
#define RECONNECT_COMMAND "reconnect"
#define SUSPEND_COMMAND	"suspend"
#define RESUME_COMMAND	"resume"

int main() {
	std::string str;
	st_service_pump service_pump;
	//st_tcp_sclient client(service_pump);
	st_sclient<st_connector_base<my_msg_buffer> > client(service_pump);
	//there is no corresponding echo client demo as server endpoint
	//because echo server with echo client made dead loop, and occupy almost all the network resource

	client.set_server_addr(SERVER_PORT + 100, SERVER_IP);
//	client.set_server_addr(SERVER_PORT, "::1"); //ipv6
//	client.set_server_addr(SERVER_PORT, "127.0.0.1"); //ipv4

	service_pump.start_service();
	while(service_pump.is_running())
	{
		std::cin >> str;
		if (str == QUIT_COMMAND)
			service_pump.stop_service();
		else if (str == RESTART_COMMAND)
		{
			service_pump.stop_service();
			service_pump.start_service();
		}
		else if (str == RECONNECT_COMMAND)
			client.graceful_close(true);
		//the following two commands demonstrate how to suspend msg sending, no matter recv buffer been used or not
		else if (str == SUSPEND_COMMAND)
			client.suspend_send_msg(true);
		else if (str == RESUME_COMMAND)
			client.suspend_send_msg(false);
		else
			client.safe_send_msg(str);
	}

	return 0;
}

//restore configuration
#undef SERVER_PORT
#undef FORCE_TO_USE_MSG_RECV_BUFFER //force to use the msg recv buffer
#undef CUSTOM_LOG
#undef DEFAULT_PACKER
#undef DEFAULT_UNPACKER

//#undef HUGE_MSG
//#undef MAX_MSG_LEN
//#undef MAX_MSG_NUM
//restore configuration

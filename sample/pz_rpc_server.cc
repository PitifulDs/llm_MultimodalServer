#include <iostream>
#include "pzmq.hpp"
#include "pzmq_data.h"
#include <string>
#include <unistd.h>  // for sleep()

using namespace StackFlows;

std::string fun1_(pzmq* self, const std::shared_ptr<pzmq_data>& msg)
{
    std::string raw_msg = msg->string();

    // 16进制打印，客户端发送的数据
    std::cout << "Raw data (hex): ";
    for (char c : raw_msg) {
        printf("%02X ", static_cast<unsigned char>(c));
    }
    std::cout << std::endl;

    // 拆包操作
    std::string param0 = msg->get_param(0);
    // 获取第二帧得数据
    std::string param1 = msg->get_param(1);

    std::cout << "fun1 received: param0=" << param0 << ", param1=" << param1 << std::endl;
    // 0x5hellosorbai
    return pzmq_data::set_param("hello", "sorbai");
}

int main(int argc, char* argv[])
{
    pzmq _rpc("test");
    _rpc.register_rpc_action("fun1", fun1_);

    while (1) {
        sleep(1);
    }
    return 0;
}
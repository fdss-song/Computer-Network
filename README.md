# TCP in the wild
SOFT130023.01 计算机网络 李景涛

## 1. 这个PJ在做什么？
1. 可靠传输、按序传输、流量控制、拥塞控制、公平（共享链路上获得大致相同的带宽）
2. 这个PJ的主要任务是实现TCP滑窗处理

## 2. TCP re-hash
1. 不同的TCP拥塞控制算法：Reno、New Reno、Cubic，，，
2. 建立连接：握手、从连接中读和写数据（socket中有buffer，多次的读和写可能会组合在一个单一的包中，或者一次读和写可能会封装到多个包中）
3. 一个例子：
	- 有两个socket，A和B，A发送数据给B
	- A会将数据存在buffer中，并且用buffer中的数据产生发送的packet，根据拥塞控制和流量控制产生尽可能多的packet
	- 当socketB接收到packet，他将数据存放在buffer中（buffer用来帮助维护按序到达）
	- B会向A发送一个ACK用来通知A我已经收到了
	- B还追踪下一个应用程序需要的字节，一旦到达，转发尽可能多的字节到应用缓冲区（按序到达）
	- 一旦包已经被B确认了，A会释放用来存储这些数据的内存
	- 最后两方可以释放链接（4次握手）
4. 实现之前理清思路，AB知道那些数据和信息；尽可能使用接口使代码模块化、高可用性

## 3. 得分点
1. **基本的TCP实现。，可靠传输，按序传递，流量控制**
2. **实现Reno拥塞控制算法**
3. **设计我们的CCA：一个数据中心或者地月通信**

## 4. 我们交什么？
1. **cmu_tcp.h**接口，其中可以加一些帮助函数，也可以更改四个核心函数（socket，close，read和write）的实现，**client.c和server.c**使用套接字发送信息，**grade.h**测试(包的长度和初始的窗口大小可能会变化)
2. 对于每一个任务都需要提供违背确认的包的数量和时间的关系，**gen_graph.h**帮助画图，为了提供一个高质量的图，这个可能需要更改

### Task1

#### Overview
1. 目标：实现滑窗，基础是停等协议
2. 分包、重组、window、重传
3. 开始代码：
	- cmu_packet.h：规定了头的基本格式，**不可更改，脚本依赖于该头文件**
	- grading.h：测试变量，不要修改，测试时会修改
	- server.c：服务器端
	- client.c：客户端
	- cmu_tcp.c：包括了socket相关的函数
	- backend.c：模拟数据缓冲和发送
	- gen_graph.py：接受一个pcap文件并且绘制出序列号随时间变化
	- cmu_packet.h：所有底层的通信都是UDP的
		-  Course Number [4 bytes]
		-  Source Port [2 bytes]
		-  Destination Port [2 bytes]
		-  Sequence Number [4 bytes]
		-  Acknowledgement Number [4 bytes] 
		-  Header Length [2 bytes] 
		-  Packet Length [2 bytes] 
		-  Flags [1 byte]
		-  Advertised Window [2 bytes] 
		-  Extension length [2 bytes]
		-  Extension Data [You Decide] 
4. **所有的整数都要按照网络字节顺序传输**(ntoh，hton)
5. **所有的整数都必须是无符号的**
6. **course number设置为15441**
7. **头文件中的域不可更改，除非第三步要扩展数据**
8. **plen≤1400**
9. **可以用wireshark或者tcpdump检测头的正确性**

#### Implement
1. [TCP握手](https://book.systemsapproach.org/e2e/tcp.html#connection-establishment-and-termination)：写在cmu中socket的构造函数和析构函数中
2. 流量控制：
	- 改变序列号和ACK数目表示发送的字节数和接受的字节数
	- 实现TCP的滑窗算法发送window size的包，使用建议的size限制发送者发送的数据
	- TCP sliding window: 
		- [1](https://book.systemsapproach.org/direct/reliable.html#sliding-window) 
		- [2](https://book.systemsapproach.org/e2e/tcp.html#sliding-window-revisited) 
	- **不必实现Nagle算法**
3. RTT估计：**用[Jacobson/Karels算法或者Karns/Partridge算法](https://book.systemsapproach.org/e2e/tcp.html#adaptive-retransmission)实现RTO的估计**
4. 重复的ACK重传：快速重传机制

### Task 2

#### Overview
1. 实现TCP Reno：正常情况下“加性增加”，丢包情况下“乘性减少”
2. **快速恢复**,TCP状态机如何进行拥塞控制

#### Implement
1. 准备：理解TCP拥塞控制的[状态机](https://intronetworks.cs.luc.edu/current/html/reno.html#tcp-reno-and-congestion-management)
2. 加入cwnd保持应用的缓冲数据总数
3. **慢启动和拥塞控制**，类似TCP Tahoe
4. **快速恢复**：TCP Tahoe-->**TCP Reno**
5. **设置带宽小于文件大小，增加丢包率**，以查看
6. **grade.h测试：**
	- WINDOW INITIAL WINDOW SIZE: 初始化窗口大小，slow start，cwnd = WINDOW INITIAL WINDOW SIZE. 
	- WINDOW INITIAL SSTHRESH: 拥塞控制中ssthresh值，slow start，ssthresh = WINDOW INITIAL SSTHRESH. 
	- MAX LEN: 包括头在内的包的最大大小，固定值，不可更改
	- MAX NETWORK BUFFER:buffer的最大大小，发送方：MAX NETWORK BUFFER 接收方：MAX NETWORK BUFFER

### Task 3
1.  实现两个场景之一：
	- 地月通信：丢包率30%，延时2.5s，最大带宽10megabit per second
	- 数据中心：丢包率0.05%，延时0.1s，最大带宽5 gigabits per second
2. 利用上面的Reno实现，传输一个20M的文件，多久完成？为什么没有在预期内完成？
3. **提交：designdiscussion.pdf中提交在选定场景中的结果，至少两段描述什么原因使其没有那么好**
4. **设计一个CCA**：比之前的实现至少快10%，**使用TCP Cubic或者TCP BBR实现**
5. **提交：designdiscussion.pdf提供我们实现的代码的运行情况**，回答以下问题：
	- 描述我们的算法，可以复现，怎么工作的，状态机是什么？
	- 我们的算法传输一个20M的文件需要多久？Reno呢？
	- 什么导致速度不同？
	- 我们的算法传输一个3M的文件需要多久？Reno呢？
	- 解释什么优于原来的算法？

## 5. 测试代码

### (1) VM
1. 安装[VirutalBox](https://www.virtualbox.org/) 和 [Vagrant](https://www.vagrantup.com/intro/getting-started/index.html)，**Vagrantfile**：在我的电脑上更改代码，或者VM上，Vagrant会自动同步
2. server和client和私密的IP地址10.0.0.1和10.0.0.2相连，要保证即便是IP地址改变了，代码仍然能正常运行，这些地址的**接口名是eth1**
3. 其他工具：文档中说明

### (2) 用tcconfig控制网络特征
1. [tcconfig]( https://github.com/thombashi/tcconfig)安装在VM上允许控制网络特征，初始值是20ms的延时（RTT=40ms），100Mbps的双向带宽
2. `$tcshow eth1` 显示参数
3. 模拟丢包、重新排序、错包

### (3) 用tcpdump和tshark抓包
1. [tcpdump](https://linux.die.net/man/8/tcpdump)和[tshark](https://www.wireshark.org/docs/man-pages/tshark.html)安装在VM上帮助抓包
2. utils中：
	- capture_packets.sh：延时如何开始和停止抓包，如何使用tshark分析
		- start函数在后台开始一个抓包的程序
		- stop函数停止抓包
		- analyze函数用tshark输出一个csv文件，包含了TCP包的头信息
	- tcp.lua：Wireshark[插件](https://mika-s.github.io/wireshark/lua/dissector/2017/11/04/creating-a-wireshark-dissector-in-lua-1.html)，以便tshark可以解析我们自定义的cmu格式
	- capture_packets.sh现实了我们如何将此文件传给tshark来解析数据包，要将插件与计算机上的Wireshark GUI一起使用，请将此文件添加到Wireshark的[插件文件夹](https://www.wireshark.org/docs/wsug_html_chunked/ChPluginFolders.)中。

### (4) pytest测试
1. [pytest](https://docs.pytest.org/en/latest/)安装在VM上，test_cp1.py测试样例
2. 我们用自己的测试
3. 标准C的debug工具，gdb 和 Valgrind也要安装在VM上

### (5) 大文件的传输
1. 传输一个100M的文件，抓包，画图，PDF提交图和pcap文件
2. 使用utils中的工具：
	- 启动TCP dump和server：
		- `vagrant@server:/vagrant/project-1$ make`
    		- `vagrant@server:/vagrant/project-1$ utils/capture_packets.sh start submit.pcap`
    		- `vagrant@server:/vagrant/project-1$ ./server`
	- 启动client：`vagrant@client:/vagrant/project-1$ ./client`
	- 传输完成，停止抓包：`vagrant@server:/vagrant/project-1$ utils/capture_packets.sh stop submit.pcap`

## 6. 提交：
- **Makefile**: Make sure all the variables and paths are set correctly such that your program compiles in the hand-in directory. Running make test should run your testing code. 
- All of your **source code files and test files**. (files ending in .c, .h, etc. only, no .o files and no executables)
- **readme.txt**: File containing a thorough description of your design and implementation. If you use any additional packet headers, please document them here. 
- **tests.txt**: File containing documentation of your test cases and any known issues you have. 
- **submit.pcap**: Your PCAP submission file from running the functionality code in server.c and client.c from the starter code (for a larger file transfer). 
- **graph.pdf**: Your graph of the currently unacked packets in flight vs time computed from submit.pcap. 
- **design.pdf**: Reflect the overall design of your project and show the function of each code module and interface.

## 7. 总评：
1. 代码
2. 风格&文档
3. balabala



## 第一次组会：2019/11/22 16:01:10 
1. 梳理文档
2. 分配任务

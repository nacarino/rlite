#############################################################################
## Table of contents                                                        #
#############################################################################

* 1. Software requirements
* 2. Build instructions
* 3. Overview of the software components
    * 3.1. Kernel modules
    * 3.2. Userspace IPCPs daemon
    * 3.3. Libraries
    * 3.4. Control tool
    * 3.5. Other tools
    * 3.6. Python bindings
* 4. Tutorials
    * 4.1 Using the demonstrator
    * 4.2 Hands-on tutorial
* 5. Configuration of IPC Processes
    * 5.1. shim-eth IPC Process
    * 5.2. shim-udp4 IPC Process
    * 5.3. shim-tcp4 IPC Process
    * 5.4. shim-loopback IPC Process
    * 5.5. Normal IPC Process


#############################################################################
## 1. Software requirements                                                 #
#############################################################################

This sections lists the software packages required to build and run *rlite* on
Linux-based operating systems. Only Ubuntu 14.04 and Archlinux are indicated
here, but using other distributions should be straightforward.

### Ubuntu 14.04 and Debian 8
#############################################################################

* gcc
* g++
* libprotobuf-dev
* protobuf-compiler
* cmake
* linux-headers-$(uname -r)
* python, swig [optional, for python bindings]

### Archlinux
#############################################################################

* gcc
* cmake
* protobuf
* linux-headers
* python, swig [optional, for python bindings]



#############################################################################
## 2. Build instructions                                                       #
#############################################################################

Download the repo and enter the root directory

    $ git clone https://github.com/vmaffione/rlite.git
    $ cd rlite

Run the configure script

    $ ./configure

Build both kernel-space and user-space software

    $ make

Install *rlite* on the system

    # make install



#############################################################################
## 3. Overview of the software components                                   #
#############################################################################

This section briefly describes the software components of *rlite*.

### 3.1. Kernel modules
#############################################################################

A main kernel module **rlite** which implements core functionalities:

* The control device for managing IPCPs, flows, registrations, etc.
* The I/O device for SDU write and read
* IPCP factories

A separate module for each type of IPCP:

* **rlite-normal**, implementing the kernel-space part of the regular IPCPs.
                    Includes EFCP and RMT.
* **rlite-shim-eth**, implementing the shim IPCP over Ethernet.
* **rlite-shim-udp4**, implementing the kernel-space part of the shim IPCP
                       over UDP and IPv4.
* **rlite-shim-tcp4**, implementing the kernel-space part of the shim IPCP
                       over TCP and IPv4. This follows an older specification
                       and it is deprecated in favour of the UDP shim IPCP.
* **rlite-shim-hv**, implementing the shim IPCP over VMPI, to be used with
                     Virtual Machines.
* **rlite-shim-loopback**, implementing a loopback shim IPCP.


### 3.2. Userspace IPCPs daemon
#############################################################################

A daemon program, **rlite-uipcps**, which implements the user-space part of
the normal IPCP, the shim-udp4, and shim-tcp4. A main thread listens on a UNIX
socket to serve incoming requests from the **rlite-ctl** control tool.
A different thread is used for each IPCP running in the system.

For the normal IPCP, uipcps daemon implements the following components:

* Enrollment
* Routing, management of lower flows and neighbors
* Application registrarion
* Flow allocation
* Codecs for RIB objects


### 3.3. Libraries
#############################################################################

Four libraries are available:

* **librlite**, the main library, wrapping the control device and I/O device.
                This is the library used by applications to register names
                and allocate flows.
* **librlite-evloop**, implementing an extensible event-loop over a control
                       device. Used by **rlite-uipcps** and the RINA
                       gateway application.
* **librlite-conf**, implementing the management and monitoring functionalities
                     of *rlite*, such as IPCP creation, removal and
                     configuration, flow monitoring, etc.
* **librlite-cdap**, a C++ implementation of the CDAP protocol.


### 3.4. Control tool
#############################################################################

The **rlite-ctl** command line tool is used for the administration of the
*rlite* stack, in the same way as the *iproute2* tool is used to administer
the Linux TCP/IP stack.

Available commands:
* ipcp-create: Create a new IPCP in the system
* ipcp-destroy: Destroy an IPCP currently running in the system
* ipcp-config: Configure an IPCP
* ipcp-register: Register an IPCP into a DIF
* ipcp-unregister: Unregister an IPCP from a DIF
* ipcp-enroll: Enroll an IPCP into a DIF
* ipcps-show: Show the list of IPCPs that are currently running in the system
* ipcp-rib-show: Show the RIB of an IPCP running in the system
* flows-show: Show the allocated flows that have a local IPCP as one of the
              endpoints

In order to show all the available command and the corresponding usage, use

    $ rlite-ctl -h


### 3.5. Other tools
#############################################################################

Other programs are available for testing and deployment:

* **rinaperf**, an application for network throughput and latency performance
                measurement. Use `rinaperf -h` to see the availble commmands.
* **rina-gw**, a deamon program implementing a gateway between a TCP/IP
               network and a RINA network.
* **rina-rr-tool**, a simple echo program written using the Python bindings.

#### Examples of rinaperf usage

Run the server, registering on a DIF called *n.DIF*:

    $ rinaperf -l -d n.DIF

Note that the rinaperf server can only manage requests one by one.

Run the client in ping mode, asking a DIF called *n.DIF* to allocate a flow:

    $ rinaperf -t ping -d n.DIF

Run the client in perf mode, asking a DIF called *n.DIF* to allocate a flow,
and using 1200 bytes sized SDUs:

    $ rinaperf -t perf -d -n.DIF -s 1200


### 3.6. Python bindings
#############################################################################

If your system runs Python, you can write applications using the *rlite*
Python bindings, which are a wrapper for the **librlite** library. Run

    >>> import rlite
    >>> help(rlite)

in the Python interpreter, in order to see the available classes.
The **rina-rr-tool** script is an example written using these bindings.



#############################################################################
## 4. Tutorials                                                             #
#############################################################################

### 4.1 Using the demonstrator
#############################################################################

The demonstrator is a tool written in Python which allows you to deploy
arbitrarily complex RINA networks, in your PC, using light Virtual Machines.

Enter the demo directory in the repository and run

    $ ./demo.py -h

to see available options.

The *rlite* demonstrator is compatible with the one
available at https://github.com/IRATI/demonstrator, which means that the
configuration files are interchangeable. The documentation contained
in the README.md file of the latter repository is still valid, with the
following differences:

1. The **policy** and **appmap** directives are not supported
2. The name of **eth** instances does not need to be a valid VLAN id
3. The legacy mode is not supported, only the buildroot mode is


#### 4.1.1 Mini-tutorial

Enter the demo directory and run

    $ ./demo.py -c demo.conf

to generate the bootstrap (up.sh) and teardown (down.sh) scripts.

Run the bootstrap script and wait for it to finish (it will take 10-20
seconds):

    $ ./up.sh

Access node **a** and run **rinaperf** in server mode:

    $ ./access.sh a
    # rlite-ctl ipcps-show  # Show the IPCPs in the system
    # rinaperf -l -d n1.DIF

Using another termial, access node **c** and run **rinaperf** in
client ping mode:

    $ ./access.sh c
    # rlite-ctl ipcps-show  # Show the IPCPs in the system
    # rinaperf -d n1.DIF -c 1000 -s 460

This will produce 1000 request/response transactions between client and server,
and the client will report the average round trip time.

Exit the node shell and teardown the scenario:

    $ ./down.sh


### 4.2 Hands-on tutorial
#############################################################################

This tutorial shows how to manually reproduce the configuration described
in demo/demo.conf, assuming that *rlite* is installed on all the three nodes.
The nodes can be realized either with physical or virtual machines.

In the demo.conf configuration, three nodes (A, B and C) are connected through
Ethernet links to form a linear topology:

    A ----- B ---- C

and a single normal DIF is stacked over the link-to-link shim DIFs.

In the following, we will assume the following local names for nodes
network interfaces:

* On node A, the interface towards B is named eth0
* On node B, the interface towards A is named eth0, while the interface
  towards C is named eth1
* On node C, the interface towards B is named eth0

An all the three nodes, load the kernel modules and run the userspace
daemon (in the example the daemon is run in foreground):

    $ sudo modprobe rlite
    $ sudo modprobe rlite-normal
    $ sudo modprobe rlite-shim-eth
    $ sudo rlite-uipcps

On node A, set-up the interface towards B and create a shim IPCP
over Ethernet:

    $ sudo ip link set eth0 up
    $ sudo rlite-ctl ipcp-create ethAB.IPCP/1// shim-eth ethAB.DIF

Bind the shim IPCP to eth0, so that the network interface will be used
to send and receive packets:

    $ sudo rlite-ctl ipcp-config ethAB.IPCP/1// netdev eth0

Create a normal IPCP and give it an address in the normal DIF:

    $ sudo rlite-ctl ipcp-create a.IPCP/1// normal n.DIF
    $ sudo rlite-ctl ipcp-config a.IPCP/1// address 71

Let the normal IPCP register to the shim DIF:

    $ sudo rlite-ctl ipcp-register ethAB.DIF a.IPCP/1//


On node B, similar operations are carried out for both the interfaces:

    $ sudo ip link set eth0 up
    $ sudo rlite-ctl ipcp-create ethAB.IPCP/1// shim-eth ethAB.DIF
    $ sudo rlite-ctl ipcp-config ethAB.IPCP/1// netdev eth0
    $
    $ sudo ip link set eth1 up
    $ sudo rlite-ctl ipcp-create ethBC.IPCP/1// shim-eth ethBC.DIF
    $ sudo rlite-ctl ipcp-config ethBC.IPCP/1// netdev eth1
    $
    $ sudo rlite-ctl ipcp-create b.IPCP/1// normal n.DIF
    $ sudo rlite-ctl ipcp-config b.IPCP/1// address 72
    $ sudo rlite-ctl ipcp-register ethAB.DIF b.IPCP/1//
    $ sudo rlite-ctl ipcp-register ethBC.DIF b.IPCP/1//

On node C:

    $ sudo ip link set eth0 up
    $ sudo rlite-ctl ipcp-create ethBC.IPCP/1// shim-eth ethBC.DIF
    $ sudo rlite-ctl ipcp-config ethBC.IPCP/1// netdev eth0
    $
    $ sudo rlite-ctl ipcp-create c.IPCP/1// normal n.DIF
    $ sudo rlite-ctl ipcp-config c.IPCP/1// address 73
    $ sudo rlite-ctl ipcp-register ethBC.DIF c.IPCP/1//

Once the IPCPs are set up, we have to carry out the enrollments in
the normal DIF. Among the possible strategies, we can enroll A and
C against B, so that B will be the initial node in the DIF.

On node A, enroll a.IPCP/1// to the neighbor b.IPCP/1// using
ethAB.DIF as a supporting DIF:

    $ sudo rlite-ctl ipcp-enroll n.DIF a.IPCP/1// b.IPCP/1// ethAB.DIF

On node C, enroll c.IPCP/1// to the neighbor b.IPCP/1// using
ethBC.DIF as a supporting DIF:

    $ sudo rlite-ctl ipcp-enroll n.DIF c.IPCP/1// b.IPCP/1// ethBC.DIF

On any node, you can check the standard output of the userspace daemon,
to check that the previous operations are completed with success.
Also the kernel log (dmesg) contains valuable log information.

It is also possible to check the list of IPCPs running in the local system:

    $ sudo rlite-ctl ipcps-show

or see the flows allocated in the local system (in this case the 0-flows
provided by the shim DIFs, which are being used by the normal DIF):

    $ sudo rlite-ctl flows-show


At this point, the setup is completed, and it is possible to run
applications on top of the normal DIF. As an example, we may run
the **rinaperf** application in server mode on node A, and the
same application in client perf mode on node C, while B will forward
the traffic.

On node A:

    $ rinaperf -l -d n.DIF

On node C:

    $ rinaperf -d n.DIF -t perf -s 1400 -c 100000



#############################################################################
## 5. Configuration of IPC Processes                                        #
#############################################################################

Each type of IPC Process has different configuration needs. shim IPC
Processes, in particular, wrap a legacy transport technology; their
configuration is closely related to the corresponding technology.


### 5.1. shim-eth IPC Process
#############################################################################

The shim DIF over Ethernet wraps an L2 Ethernet network. A shim-eth IPCP
must be configured with the O.S. name of the Ethernet Network Interface Card
(NIC) that is attached to the network.

In the following example

    $ sudo rlite-ctl ipcp-config ether3/181// netdev eth2

a shim IPCP called ether3/181// is assigned a network interface called eth2.


### 5.2. shim-udp4 IPC Process
#############################################################################

The shim DIF over UDP/IPv4 wraps an arbitrary IPv4 network that supports UDP
as a transport protocol. As a lower level mechanisms, regular UDP sockets are
used to transmit/receive PDUs. For an application to use (register, allocate
flows) this shim DIF, a mapping must be defined between IP addresses and
application name. Each IP address univocally identifies a network interface
of a node in the shim IPCP, and therefore it also univocally identifies the
node itself. An IP address must be mapped to a single application name, so
that all flow allocation requests (UDP packets) arriving to that IP are
forwarded to that application. The mappings must be stored in the standard
/etc/hosts file of each node taking part in the shim DIF, or in a DNS
server.

An example of /etc/hosts configuration is the following:

    127.0.0.1       localhost.localdomain   localhost
    ::1             localhost.localdomain   localhost
    8.12.97.231     xyz-abc--
    8.12.97.230     asd-63--

In this example, the IP 8.12.97.231 is mapped to an application called
xyz/abc//, while the IP 8.12.97.230 is mapped to another application
called asd/63//. This means that this shim UDP implements a tunnel
between two nodes. The first endpoint node has a network interface configured
with the address 8.12.97.231 (with some netmask), and a RINA application
called xyz/abc// can register to the local shim UDP IPCP. The other endpoint
node has a network interface configured with the address 8.12.97.232, and a
RINA application called asd/63// can register to the local shim UDP IPCP.

Note that while an IP address corresponds to one and only one application
name, an application name may correspond to multiple IP addresses. This
simply means that the same application is available at different network
interfaces (which could be useful for load balancing and high availability).

The /etc/hosts file (or DNS records) must be configured before any application
registration or flow allocation operation can be performed.
The current implementation does not dynamically update the
/etc/hosts file nor the DNS servers. Configuration has to be done
statically. This is not usually a real limitation, since you may probably
want to use the shim UDP to create a tunnel (over the Internet) between two
or a few RINA-only networks, in a VPN-like fashion. In this case a few lines
in /etc/hosts on each host which act as a tunnel endpoints will suffice.

Note that because of its nature, a single shim UDP IPCP for each node is
enough for any need. In other words, creating more shim IPCPs on the same node
is pointless.


### 5.3. shim-tcp4 IPC Process
#############################################################################

In spite of the name being similar, the shim DIF over TCP/IPv4 is fundamentally
different from its UDP counterpart. While the name of an application running
over the shim UDP is mapped to an IP address, the name of an application
running over the shim TCP is mapped to a couple (IP address, TCP port).
The difference is explained by the fact that the shim UDP automatically
allocates a new local UDP port for each flow to allocate.
Nevertheless, both shims use sockets as an underlying transport technology,
and the use cases are similar.

As a consequence, the configuration for the shim TCP is not specified using
a standard configuration file (e.g. /etc/hosts). An ad-hoc configuration
file is stored at /etc/rlite/shim-tcp4-dir.

An example configuration is the following:

    rinaperf-data/client// 10.0.0.1 6789 i.DIF
    rinaperf-data/server// 10.0.0.2 6788 i.DIF

where the application named rinaperf-data/client// is mapped (bound) to the
TCP socket with address 10.0.0.1:6789 and rinaperf-data/server// is mapped
to the TCP socket 10.0.0.1:6788. These mappings are valid for a shim DIF
called i.DIF.

Note that the shim DIF over UDP should be preferred over the TCP one, for
two reasons:
    - Configuration does not use a standard file, and allocation of TCP ports
      must be done statically.
    - SDU serialization is needed, since TCP is not message (datagram)
      oriented, but stream oriented; SDU length has to be encoded in the
      stream, and this adds overhead and is more error prone
    - TCP handshake, retransmission and flow control mechanism add overhead
      and latency, introduces latency; moreover, these tasks should be
      carried out by EFCP.

In conclusion, the shim TCP is to be considered legacy, and future developments
are not expected to focus on it. It is strongly recommended to always use the
UDP shim when interfacing *rlite* with IP networks.


### 5.4. shim-loopback IPC Process
#############################################################################

The shim-loopback conceptually wraps a loopback network device. SDUs sent on
a flow supported by this shim are forwarded to another flow supported by the
same shim. It is mostly used for testing purpose and as a stub module for
the other software components, since the normal IPCP support the
same functionalities (i.e. self-flows). However, it may be used for local
IPC without the need of the uipcp server.

It supports two configuration parameter:
 * **queued**: if 0, SDUs written are immediately forwarded (e.g. in process
    context to the destination flow; if different from 0, SDUs written are
    fowarded in a deferred context (a Linux workqueue in the current
    implementation).
 * **drop_fract**: if different from 0, an SDU packet is dropped every
                    **drop_fract** SDUs.


### 5.5. Normal IPC Process
#############################################################################

In the current implementation a normal IPC Process needs to be configured
with an address which is unique in its DIF. This step will eventually
become unnecessary (and not allowed anymore), once a first address space
management strategy is implemented for the normal DIF (e.g. distributed or
centralized address allocation).

In the following example

    $ sudo rlite-ctl ipcp-config normal1/xyz// address 7382

a normal IPCP called normal1/xyz// is given the address 7382 to be used
in its DIF.


#

### Testing

Ping lwIP directly `docker exec -it dtn_node_2 ping6 fd00::2`

Ping lwIP directly `docker exec -it dtn_node_1 ping6 fd00:22::2`

check pings `docker exec -it dtn_node_1 tcpdump -i tun0 -n icmp6`


##### node 0 -> node 1

docker exec -it reg_node_0 ping6 -c 1 fd00:0:1::1

docker exec -it reg_node_0 ping6 -c 1 fd00:1:2::1

docker exec -it reg_node_0 ping6 -c 1 fd00:2:3::1

##### node 0 <- node 1
docker exec -it dtn_node_1 ping6 -c 1 fd00:0:1::1

docker exec -it dtn_node_1 ping6 -c 1 fd00:1:2::2

##### node 0 -> node 2
docker exec -it reg_node_0 ping6 -c 1 fd00:1:2::2

##### node 0 -> node 3
docker exec -it reg_node_0 ping6 -c 1 fd00:2:3::2

##### node 1 -> node 0
docker exec -it dtn_node_3 ping6 -c 1 fd00:0:1::1

##### node 2 -> node 3
docker exec -it dtn_node_2 ping6 -c 1 fd00:2:3::2

docker exec -it dtn_node_3 ping6 -c 1 fd00:ffff:3::2
docker exec -it dtn_node_3 ping6 -c 1 fd00:2:3::2

docker exec -it dtn_node_3 /bin/sh ./capture.sh

### listen to traffic inside node

docker exec -it dtn_node_3 bash

# Then inside:
tcpdump -i enp0s8 -n ip6     # traffic from node 0
tcpdump -i enp0s9 -n ip6     # traffic toward node 2
tcpdump -i any -n ip6         # all interfaces



####



docker exec -it dtn_node_3 bash
root@cef96fc77107:~# tcpdump -i any -n ip6
tcpdump: data link type LINUX_SLL2
tcpdump: verbose output suppressed, use -v[v]... for full protocol decode
listening on any, link-type LINUX_SLL2 (Linux cooked v2), snapshot length 262144 bytes
17:19:41.984927 enp0s8 In  IP6 fd00:0:1::1 > fd00:2:3::2: ICMP6, echo request, id 1, seq 1, length 64
17:19:41.984944 tun0  Out IP6 fd00:0:1::1 > fd00:2:3::2: ICMP6, echo request, id 1, seq 1, length 64
17:19:41.985028 enp0s8 Out IP6 fd00:ffff:3::2 > fd00:0:1::1: ICMP6, mtrace response, length 65
        0x0000:  c800 a973 0000 0000 3704 0100 0000 0040
        0x0010:  0060 0ade 9f00 403a 3dfd 0000 0000 0100
        0x0020:  0000 0000 0000 0000 01fd 0000 0200 0300
        0x0030:  0000 0000 0000 0000 0280 00d0 2600 0100
        0x0040:  01
17:19:41.985061 tun0  In  IP6 fd00:0:1::1 > fd00:2:3::2: ICMP6, echo request, id 1, seq 1, length 64
17:19:41.985071 tun0  Out IP6 fd00:2:3::2 > fd00:0:1::1: ICMP6, echo reply, id 1, seq 1, length 64
17:19:41.985104 enp0s8 Out IP6 fd00:2:3::2 > fd00:0:1::1: ICMP6, echo reply, id 1, seq 1, length 64


17:31:08.868610 enp0s8 In  IP6 fd00:0:1::1 > fd00:2:3::2: ICMP6, echo request, id 3, seq 1, length 64
17:31:08.868619 tun0  Out IP6 fd00:0:1::1 > fd00:2:3::2: ICMP6, echo request, id 3, seq 1, length 64
17:31:08.868710 tun0  In  IP6 fd00:0:1::1 > fd00:2:3::2: ICMP6, echo request, id 3, seq 1, length 64
17:31:08.868730 tun0  Out IP6 fd00:2:3::2 > fd00:0:1::1: ICMP6, echo reply, id 3, seq 1, length 64
17:31:08.868753 enp0s8 Out IP6 fd00:2:3::2 > fd00:0:1::1: ICMP6, echo reply, id 3, seq 1, length 64


docker exec -it dtn_node_1 bash

docker exec -it dtn_node_1 tcpdump -i any -n ip6


docker exec -it reg_node_0 ping6 -c 1 fd00:2:3::2



docker exec -it dtn_node_3 watch -n1 'ip6tables -t mangle -L PREROUTING -v'

 pkts bytes target     prot opt in     out     source               destination
    3   168 ACCEPT     ipv6-icmp --  any    any     anywhere             anywhere             ipv6-icmp router-solicitation
    0     0 ACCEPT     ipv6-icmp --  any    any     anywhere             anywhere             ipv6-icmp router-advertisement
    6   432 ACCEPT     ipv6-icmp --  any    any     anywhere             anywhere             ipv6-icmp neighbour-solicitation
    0     0 ACCEPT     ipv6-icmp --  any    any     anywhere             anywhere             ipv6-icmp neighbour-advertisement
   24  2740 MARK       all  --  enp0s8 any     anywhere             anywhere             MARK set 0x1


docker exec -it dtn_node_1 tcpdump -i enp0s9 -n icmp6

docker exec -it dtn_node_3 ip -6 neigh show     

docker exec -it reg_node_0 tcpdump -i enp0s9 -n icmp6

docker exec -it dtn_node_2 tcpdump -i enp0s8 -n icmp6


docker exec -it reg_node_0 ping6 -c 4 fd00:12::1


### 

#### Show routes
docker exec reg_node_0 ip -6 route show

docker exec dtn_node_1 ip -6 route show

docker exec dtn_node_2 ip -6 route show

(ml1) tiredbluewhale@Kiste lwIP-tun-dtn % docker exec reg_node_0 ip -6 route show
fd00:1::/64 dev enp0s9 proto kernel metric 256 pref medium
fd00:11::/64 via fd00:1::2 dev enp0s9 metric 1024 pref medium
fd00:12::/64 via fd00:1::2 dev enp0s9 metric 1024 pref medium
fd00:22::/64 via fd00:1::2 dev enp0s9 metric 1024 pref medium
fd00:23::/64 via fd00:1::2 dev enp0s9 metric 1024 pref medium
fe80::/64 dev enp0s9 proto kernel metric 256 pref medium
(ml1) tiredbluewhale@Kiste lwIP-tun-dtn % docker exec dtn_node_2 ip -6 route show
fd00:1::/64 via fd00:12::1 dev enp0s8 metric 1024 pref medium
fd00:11::/64 via fd00:12::1 dev enp0s8 metric 1024 pref medium
fd00:12::/64 dev enp0s8 proto kernel metric 256 pref medium
fd00:22::/64 dev tun0 proto kernel metric 256 pref medium
fd00:23::/64 dev enp0s9 proto kernel metric 256 pref medium
fd00:33::/64 via fd00:23::3 dev enp0s9 metric 1024 pref medium
fe80::/64 dev enp0s8 proto kernel metric 256 pref medium
fe80::/64 dev enp0s9 proto kernel metric 256 pref medium
fe80::/64 dev tun0 proto kernel metric 256 pref medium
(ml1) tiredbluewhale@Kiste lwIP-tun-dtn % docker exec dtn_node_1 ip -6 route show
fd00:1::/64 dev enp0s8 proto kernel metric 256 pref medium
fd00:11::/64 dev tun0 proto kernel metric 256 pref medium
fd00:12::/64 dev enp0s9 proto kernel metric 256 pref medium
fd00:22::/64 via fd00:12::2 dev enp0s9 metric 1024 pref medium
fd00:23::/64 via fd00:12::2 dev enp0s9 metric 1024 pref medium
fd00:33::/64 via fd00:12::2 dev enp0s9 metric 1024 pref medium
fe80::/64 dev enp0s8 proto kernel metric 256 pref medium
fe80::/64 dev enp0s9 proto kernel metric 256 pref medium
fe80::/64 dev tun0 proto kernel metric 256 pref medium

####

docker exec -it reg_node_0 ping6 fd00:22::2

docker exec -it dtn_node_1 ip6tables -t mangle -L PREROUTING -n -v
Chain PREROUTING (policy ACCEPT 0 packets, 0 bytes)
 pkts bytes target     prot opt in     out     source               destination         
    3   168 ACCEPT     58   --  *      *       ::/0                 ::/0                 ipv6-icmptype 133
    0     0 ACCEPT     58   --  *      *       ::/0                 ::/0                 ipv6-icmptype 134
   21  1512 ACCEPT     58   --  *      *       ::/0                 ::/0                 ipv6-icmptype 135
   12   768 ACCEPT     58   --  *      *       ::/0                 ::/0                 ipv6-icmptype 136
   15  1880 MARK       0    --  enp0s86 *       ::/0                 ::/0                 MARK set 0x1
   69  7395 MARK       0    --  enp0s96 *       ::/0                 ::/0                 MARK set 0x1

docker exec -it dtn_node_1 ip -6 rule show
0:      from all lookup local
10000:  from all fwmark 0x1 lookup 100
32766:  from all lookup main

docker exec -it dtn_node_1 ip -6 route show table 100
default via fd00::2 dev tun0 metric 1024 pref medium

### Testing

`LwIP: docker exec -it dtn_node_2 ping6 fd00:11::2`

`LwIP: docker exec -it dtn_node_1 ping6 fd00:22::2`

`LwIP: docker exec -it dtn_node_2 ping6 fd00:12::2`

docker exec -it dtn_node_2 ping6 fd00:00::2


## Docker

# docker build -f Dockerfile.build  -t dtn_node_build .

docker build -f Dockerfile.dev .

docker build -t lwip-tun-dtn .

docker-compose -f docker-compose-test.yml down


docker run -it -d --rm --cap-add=NET_ADMIN --device /dev/net/tun lwip-tun-dtn /bin/bash

docker run -it --name lwip-tun-dtn --rm --cap-add=NET_ADMIN --device /dev/net/tun lwip-tun-dtn

docker exec -it dtn_node_1 sh

docker exec -it dtn_node_2 sh


# node 1 -> 2
ping6 fd00:12::1

ping6 fd00:12::2

tcpdump -ni enp0s9

# node 1 TUN address 
ping6 fd00::1

# This packet should be MARKED by iptables and routed to TUN0
ping6 fd00:23::1

ping6 fd00:32::1

docker build -t lwip-tun-dtn .

# container
docker rm -vf $(docker ps -a -q)
# images
docker rmi -f $(docker images -a -q)

ip -6 neigh add fd00:12::2 lladdr 6a:31:67:02:38:54 dev eth0

ip -6 neigh add fd00:12::1 lladdr 5e:84:75:60:bd:74 dev eth0


docker-compose down --remove-orphans

docker run -it --rm \
    --privileged \
    --device /dev/net/tun \
    --name node1_lwip \
    lwip-tun-dtn


docker run -it --privileged --entrypoint /bin/bash lwip-tun-dtn

## Node 1
interfaces
fd00:0001::1
fd00:0001::2
fd00:0001::3

lwIP
fd00:0001::100
fd00:0001::101

## Node 2
interfaces
fd00:0002::1
fd00:0002::2

lwIP
fd00:0002::100
fd00:0002::101

## Node 3
interfaces
fd00:0003::1

lwIP
fd00:0003::100
fd00:0003::101

## Network

node 1 <1-1> node 2 <2-1> node 3


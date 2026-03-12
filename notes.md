#

##

### build 

# docker build -f Dockerfile.build  -t dtn_node_build .

docker build -f Dockerfile.build .

docker build -t lwip-tun-dtn .


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
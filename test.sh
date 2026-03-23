MAC1=$(docker exec dtn_node_1 ip link show enp0s9 | awk '/ether/{print $2}')
MAC2=$(docker exec dtn_node_2 ip link show enp0s8 | awk '/ether/{print $2}')

echo "Node1: $MAC1  Node2: $MAC2"

docker exec dtn_node_1 ip -6 neigh add fd00:12::2 lladdr $MAC2 dev enp0s9 nud permanent
docker exec dtn_node_2 ip -6 neigh add fd00:12::1 lladdr $MAC1 dev enp0s8 nud permanent

docker exec -it dtn_node_2 ping6 -c3 fd00:12::1
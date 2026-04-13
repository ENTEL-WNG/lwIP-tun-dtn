echo "Node 0 -> Node 1"
docker exec -it reg_node_0 ping6 -c 1 fd00:0:1::2

echo "Node 0 -> Node 3"
docker exec -it reg_node_0 ping6 -c 1 fd00:2:3::2

echo "Node 3 -> Node 0"
docker exec -it dtn_node_3 ping6 -c 1 fd00:0:1::1

docker exec -it dtn_node_1 ping6 -c 1 fd00:2:3::2
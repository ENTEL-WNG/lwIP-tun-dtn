echo "Node 1 -> Node 2 "
docker exec -it dtn_node_2 ping6 -c3 fd00:12::2

echo "Node 2 -> Node 1 "
docker exec -it dtn_node_2 ping6 -c3 fd00:12::1

echo "Node 1 -> Node 2 "
docker exec -it dtn_node_2 ping6 -c3 fd00:12::1
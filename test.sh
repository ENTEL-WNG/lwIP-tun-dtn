echo "Node 0 -> Node 1"
docker exec -it reg_node_0 ping6 -c 1 fd00:0:1::2

echo "Node 0 -> Node 3"
docker exec -it reg_node_0 ping6 -c 1 fd00:2:3::2

echo "Node 3 -> Node 0"
docker exec -it dtn_node_3 ping6 -c 1 fd00:0:1::1

echo "Node 1 -> Node 3"
docker exec -it dtn_node_1 ping6 -c 1 fd00:2:3::2

echo "UDP Node 0 -> Node 3"
docker exec -it reg_node_0 sh -c "echo 'Hello from plain eagle' | nc -u -w 1 fd00:2:3::2 8080"


docker exec -it dtn_node_3 nc -u -l -p 8080


docker exec -it reg_node_0 sh echo "hello" | nc -u fd00:2:3::2 8080


nc -u -6 -l -k -p 8080

docker exec -it reg_node_0  nc -u -6 fd00:2:3::2 8080

echo "hi" | nc -u -6 fd00:2:3::2 8080
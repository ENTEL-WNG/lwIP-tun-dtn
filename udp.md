# UDP

listener: `nc -u -6 -l -k -p 8080`
receiver: `nc -u -6 fd00:2:3::2 8080`


listener: `docker exec -it dtn_node_3 nc -u -6 -l -k -p 8080
receiver: `docker exec -it reg_node_0 nc -u -6 fd00:2:3::2 8080`
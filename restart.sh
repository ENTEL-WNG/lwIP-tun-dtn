docker compose down
# docker rmi -f $(docker images -a -q)
docker network prune -f
docker compose up --build

# docker compose up --build
# docker builder prune
#!/bin/bash

docker compose -f tests/docker-compose.test.yml up --build --abort-on-container-exit
docker compose -f tests/docker-compose.test.yml down --volumes --remove-orphans
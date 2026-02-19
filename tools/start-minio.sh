
#!/bin/bash
docker run -d \
  --name minio \
  -p 9000:9000 \
  -p 9001:9001 \
  -e MINIO_ROOT_USER=minioadmin \
  -e MINIO_ROOT_PASSWORD=minioadmin \
  -v ~/minio/data:/data \
  quay.io/minio/minio server /data --console-address ":9001"

sleep 1

docker exec -it minio \
  sh -c "
  mc alias set local http://127.0.0.1:9000 minioadmin minioadmin &&
  mc mb local/reprobuild
  "

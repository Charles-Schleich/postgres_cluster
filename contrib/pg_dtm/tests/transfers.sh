#!/bin/sh

go run transfers.go \
	-d 'dbname=postgres port=5432' \
	-d 'dbname=postgres port=5433' \
	-v \
	-m \
	-u 100000 \
	-w 8 \
	-g

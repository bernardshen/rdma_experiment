all: client server ud_pingpong

server: server.c ib.c
	cc server.c ib.c -libverbs -o server -g

client: client.c ib.c
	cc client.c ib.c -libverbs -o client -g

ud_pingpong: ud_pingpong.c pingpong.c
	cc ud_pingpong.c pingpong.c -libverbs -o ud_pingpong -g

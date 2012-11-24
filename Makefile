all: tun2udp tun2fifo

tun2udp: tun2udp.c *.h
	cc -o tun2udp tun2udp.c

tun2fifo: tun2fifo.c *.h
	cc -o tun2fifo tun2fifo.c

clean:
	rm -f tun2udp tun2fifo

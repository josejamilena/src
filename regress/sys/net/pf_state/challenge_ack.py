#!/usr/local/bin/python2.7
# check wether path mtu to dst is as expected

import os
import threading
from addr import *
from scapy.all import *

#
# we can not use scapy's sr() function as receive side
# ignores the packet we expect to see. Packet is ignored
# due to mismatching sequence numbers. 'bogus_syn' is using
# seq = 1000000, while response sent back by PF has ack,
# which fits regular session opened by 'syn'.
#
class Sniff1(threading.Thread):
	filter = None
	captured = None
	packet = None
	def run(self):
		self.captured = sniff(iface=LOCAL_IF, filter=self.filter,
		    count=1, timeout=5)
		if self.captured:
			self.packet = self.captured[0]

fake_port=os.getpid() & 0xffff

ip=IP(src=FAKE_NET_ADDR, dst=REMOTE_ADDR)

print "Send SYN packet, receive SYN+ACK"
syn=TCP(sport=fake_port, dport='echo', seq=1, flags='S', window=(2**16)-1)
syn_ack=sr1(ip/syn, iface=LOCAL_IF, timeout=5)

if syn_ack is None:
	print "ERROR: no matching SYN+ACK packet received"
	exit(1)

print "Send ACK packet to finish handshake."
ack=TCP(sport=syn_ack.dport, dport=syn_ack.sport, seq=2, flags='A',
    ack=syn_ack.seq+1)
send(ip/ack, iface=LOCAL_IF)

print "Connection is established, send bogus SYN, expect challenge ACK"
bogus_syn=TCP(sport=fake_port, dport='echo', seq=1000000, flags='S',
    window=(2**16)-1)
sniffer = Sniff1();
sniffer.filter = "src %s and tcp port echo and dst %s and tcp port %u " \
    "and tcp[tcpflags] = tcp-ack" % (REMOTE_ADDR, FAKE_NET_ADDR, fake_port)
sniffer.start()
time.sleep(1)
send(ip/bogus_syn, iface=LOCAL_IF)
sniffer.join(timeout=7)
challenge_ack = sniffer.packet

if challenge_ack is None:
	print "ERROR: no matching ACK packet received"
	exit(1)

if challenge_ack.getlayer(TCP).seq != (syn_ack.seq + 1):
	print "ERROR: expecting seq %d got %d in challange ack" % \
	    (challenge_ack.getlayer(TCP).seq, (syn_ack.seq + 1))
	exit(1)

exit(0)

#!/usr/bin/env python3

"""
   Stress test the infod store by
   generating pseudo-random get/put/delete operations.
   Connects to a running infod instance over AF_UNIX.

   Usage: stress.y [pause-at]

   To use, first run "infod -f /tmp/info.store" in the background.
   Then run this test script and wait for it to complete.
   (It will first delete all keys in the info store)
"""

CMD_SUB =   0x01
CMD_UNSUB = 0x02
CMD_READ =  0x03
CMD_WRITE = 0x04
CMD_BEGIN = 0x05
CMD_COMMIT= 0x06
CMD_PING  = 0x07
MSG_INFO  = 0x81
MSG_PONG  = 0x82
MSG_ERROR = 0x83

class Info:
	""" Simple client connection to infod.
	       i = Info()
	       i["foo"] = b'bar'    -- write
	       i["foo"]             -- read
	       del i["foo"]         -- delete
	       i.all()              -- sub *
	"""
	def __init__(self):
		from socket import socket, AF_UNIX, SOCK_SEQPACKET
		self.s = socket(AF_UNIX, SOCK_SEQPACKET, 0)
		self.s.connect(b'\0INFOD')
	def send(self, cmd, data = b''):
		self.s.send(bytes([cmd]) + data)
	def recv(self):
		b = self.s.recv(65536)
		return int(b[0]), b[1:]
	def __getitem__(self, name):
		key = bytes(name, "utf-8")
		keylen = len(key)
		self.send(CMD_READ, key)
		msg,data = self.recv()
		assert (msg == MSG_INFO
		   and  data[:keylen] == key
		   and  (len(data) == keylen or data[keylen] == 0)
		       ), "%02x:%s" % (msg, repr(data))
		if len(data) == keylen:
			return None
		return data[keylen+1:]
	def __setitem__(self, name, value):
		key = bytes(name, "utf-8")
		assert isinstance(value, bytes)
		self.send(CMD_WRITE, key + b'\0' + value)
	def __delitem__(self, name):
		key = bytes(name, "utf-8")
		self.send(CMD_WRITE, key)
	def all(self):
		accum = []
		self.send(CMD_BEGIN)
		self.send(CMD_SUB, b'*')
		self.send(CMD_UNSUB, b'*')
		self.send(CMD_PING)
		self.send(CMD_COMMIT)
		while True:
			msg,data = self.recv()
			if msg == MSG_PONG: break
			assert msg == MSG_INFO
			p = data.find(0)
			accum.append((str(data[:p], "utf-8"), data[p+1:]))
		return accum


import random

info = Info()

print("deleting existing keys")
for key,value in info.all():
	del info[key]

# A variety of big and small keys and values. Our limit of key+value is 64kB,
# so keep keys and values under <32kB each. Also, the page size is 4kB
# and the element alignment is 8 bytes, so legths have been chosen that might
# exercise boundary conditions on combinations.
KEYS = ['a', 'b', 'aa', 'foo', 'bar', '999999999',
  'this is a relatively short key',
  'this key is so long that you think it will never end but actually it does '+
  'but it just takes a really long time but it turns out to be much longer ' +
  'than you expected, unless of course youve already looked at the key and ' +
  'you know exactly how long it is, or you have an idea to expect that when ' +
  'a key is describing itself as really long then it will probably will be',
  'K' * 30000]
VALUES = [b'', b'x', b'xx', b'xxx', b'999999999',
  b'supercalafragilisticexpialadocious',
  b'qwertyuiopasdfghjklzxcvbnm1234567890-=[];,./MNBVCXZLKJHGFDSAPOIUYTREWQ',
  b'V' * (65534-30000) ]
PUT_OP = 'put'
DEL_OP = 'del'
OPS = (PUT_OP, PUT_OP, PUT_OP, PUT_OP, PUT_OP, DEL_OP)

from sys import argv
if len(argv) > 1:
	pause_at = int(argv[1])
else:
	pause_at = None

rand = random.Random(0)
sim = {}
print("exercising",)
for x in range(1000000):
	try:
		op = rand.choice(OPS)
		if op is PUT_OP:
			key = rand.choice(KEYS)
			value = rand.choice(VALUES)
		elif op is DEL_OP:
			key = rand.choice(KEYS)
			value = None

		if pause_at is not None and x == pause_at:
			print("pausing at x =", x)
			print("sim:")
			for k in sorted(sim.keys()):
			    print("  ",k,"=",repr(sim[k]))
			print("next op =", op, ", key =", key,
				", value =", repr(value))
			input("paused at %s; press enter to continue:" % x)
		if (x % 10000) == 0:
			print(("%3d%%" % (x / 10000)), end='\r', flush=True)
			# check that sim == info
			ia = info.all()
			assert len(ia) == len(sim),  "ia=%r sim=%r" % (ia, sim)
			for k,v in ia:
				assert k in sim, ("k=%r" % k)
				assert sim[k]==v, ("sim[k]=%r v=%r"%(sim[k],v))

		# perform the action on both infod and the simulation dict
		if op is PUT_OP:
			info[key] = value
			sim[key] = value
		elif op is DEL_OP:
			del info[key]
			if key in sim: del sim[key]

		# synchronize; otherwise we fill up the buffer with sends
		info.send(CMD_PING)
		msg,data = info.recv()
		assert msg == MSG_PONG

	except:
		print("exception at x =", x)
		raise

# Final check that the simulation and infod agree
ia = info.all()
assert len(ia) == len(sim),  "ia=%r sim=%r" % (ia, sim)
for k,v in ia:
	assert k in sim, ("k=%r" % k)
	assert sim[k] == v, ("sim[k]=%r v=%r" % (sim[k],v))
print("100% no errors detected")

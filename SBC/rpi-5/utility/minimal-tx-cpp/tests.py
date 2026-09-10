#!/usr/bin/env python3
"""Host-only checks: no radio device opens or control ioctls."""
# The tests use only Python standard-library modules. The C++ program is
# run as a separate process, so checks exercise parsing, generation and writing
# together rather than calling private C++ helpers.
import pathlib, struct, subprocess, sys, tempfile
# Resolve before entering temporary directories so relative paths still work.
exe=str(pathlib.Path(sys.argv[1]).resolve())
# Count assertions as well as successful generator runs; this is why the
# reported check count is larger than the number of waveform examples.
checks=0
# Capture stdout/stderr for failure diagnostics and bound each process to
# ten seconds so an accidental loop cannot hang the host test indefinitely.
def run(*a):return subprocess.run([exe,*map(str,a)],capture_output=True,timeout=10)
# Raise with a useful label immediately on failure. A successful run increments
# the shared count. No optimisation flag should disable these Python assertions.
def require(ok,label):
 global checks
 assert ok,label
 checks+=1
# The context manager removes generated files when the test exits, including
# after a failed assertion. Existing user files are never used as fixtures.
with tempfile.TemporaryDirectory(prefix='radioberry-tests-') as tmp:
 root=pathlib.Path(tmp)
 # Run one valid CLI case and return the actual binary output. Each case uses
 # a different path because the program deliberately refuses overwrites.
 def output(name,bits,*a):
  p=root/name;r=run(bits,'--output',p,*a)
  require(r.returncode==0,r.stderr.decode());return p.read_bytes()
 # Independent oracle: Python packs signed 16-bit I and Q directly in big-endian
 # order. It does not duplicate the C++ signed-to-unsigned shift expression.
 # Multiply bytes to hold each symbol, concatenate repeats, then append zero tail.
 def reference(bits,sps,amp,repeat=1,tail=0):
  b=b''.join(struct.pack('>hh',amp if c=='1' else -amp,0)*sps for c in bits*repeat)+bytes(tail*4)
  return b+bytes((-len(b))%16384)
 # Default eight-bit case checks complete payload bytes and every padding byte.
 require(output('a.iq','10100101')==reference('10100101',48,4096),'BPSK and zero padding')
 # 701 samples per bit deliberately makes a 4096-word block boundary fall
 # inside a symbol. This catches restarting generation at each block.
 require(output('b.iq','010','--samples-per-bit',701,'--repeat',3,'--amplitude',32767,'--tail-samples',123)==reference('010',701,32767,3,123),'Symbol and message continuity across blocks')
 # An exact block must not acquire another block of padding.
 require(output('c.iq','1','--samples-per-bit',4096,'--amplitude',1)==reference('1',4096,1),'Exact block no extra padding')
 # One tail word beyond a full payload block requires a whole second block.
 require(output('d.iq','1','--samples-per-bit',4096,'--tail-samples',1)==reference('1',4096,4096,1,1),'Tail creates next block')
 # DEADBEEF has mixed bits in both 16-bit halves, exposing reversed byte/bit
 # order. 4097 repeated words cross the block boundary by one word.
 raw='11011110101011011011111011101111'
 require(output('raw.iq',raw,'--mode','raw','--repeat',4097)==bytes.fromhex('deadbeef')*4097+bytes(32768-4097*4),'Raw bit order across blocks')
 # All-zero and all-one inputs exercise both extremes of the raw word range.
 require(output('raw2.iq','0'*32+'1'*32,'--mode','raw')==bytes(4)+b'\xff'*4+bytes(16376),'Zero and all-ones raw words')
 # Make a small existing file and verify a failed creation preserves its bytes.
 existing=root/'existing.iq';existing.write_bytes(b'keep me')
 require(run('1','--output',existing).returncode!=0 and existing.read_bytes()==b'keep me','Do not overwrite')
 # Exercise validation before I/O: invalid alphabet, raw word length, modes,
 # zero/negative/out-of-range numbers, overflow, conflicting file options and
 # malformed control fields. These inputs must never create output files.
 invalid=[('',),('1012',),('10 01',),('-1',),('1','--mode','raw'),('1','--mode','other'),('1','--samples-per-bit','0'),('1','--repeat','0'),('1','--amplitude','32768'),('1','--amplitude','0'),('1','--sample-rate','0'),('1','--repeat','12x'),('1','--repeat','-1'),('1','--tail-samples','18446744073709551615'),('11','--samples-per-bit','18446744073709551615'),('1','--repeat','18446744073709551616'),('1','--configured'),('1','--hold-ms','0'),('1','--start-control','100:0:0'),('1','--start-control','0:0:100000000'),('1','--start-control','0:0:zz'),('1','--start-control','0:0:0:0'),('1','--unknown','4')]
 # Every invalid case gets a unique destination. A nonzero exit alone is not
 # enough: assert that no file was created before the argument failure.
 for index,args in enumerate(invalid):
  p=root/f'bad-{index}.iq';r=run(*args,'--output',p)
  require(r.returncode!=0 and not p.exists(),f'Reject before output: {args}')
 # These device cases fail before radio access. The only existing destination
 # used below is our temporary REGULAR file, which must be rejected unchanged.
 missing=root/'no-device'
 require(run('1','--device',missing).returncode!=0,'Hold required')
 require(run('1','--device',missing,'--hold-ms',0).returncode!=0,'Control mode required')
 require(run('1','--device',missing,'--hold-ms',0,'--configured','--start-control','0:0:0').returncode!=0,'Mixed controls rejected')
 require(run('1','--device',existing,'--hold-ms',0,'--configured').returncode!=0 and existing.read_bytes()==b'keep me','Device must be character device')
 # Help is the one successful invocation without a bit string.
 require(run('--help').returncode==0,'Help')
 require(run().returncode!=0,'Missing arguments')
# Only reached after every assertion passed. This reports host behaviour,
# not ioctl correctness, hardware timing or RF completion.
print(f'PASS: {checks} host-only checks')

//use with WifiShieldOSC.ino
//f.olofsson2019
(
n= NetAddr("OpenBCI_WifiShieldOSC.local", 13999);
OSCdef(\ready, {|msg| msg.postln}, \ready, n);
OSCdef(\reply, {|msg| msg.postln}, \reply, n);
OSCdef(\version, {|msg| msg.postln}, \version, n);
OSCdef(\name, {|msg| msg.postln}, \name, n);
OSCdef(\board, {|msg| msg.postln}, \board, n);
OSCdef(\all, {|msg| msg.postln}, \all, n);
OSCdef(\data, {|msg| msg.postln}, \data, n);
)

n.sendMsg(\reset);
n.sendMsg(\start);
n.sendMsg(\stop);

n.sendMsg(\version);
n.sendMsg(\name);
n.sendMsg(\board);
n.sendMsg(\all);

n.sendMsg(\command, "~~");  //get samplerate
n.sendMsg(\command, "~4");  //set samplerate to 1000Hz
n.sendMsg(\command, "$1");  //turn channel 1 off
n.sendMsg(\command, "$!");  //turn channel 1 on
//etc.
//see https://docs.openbci.com/OpenBCI%20Software/04-OpenBCI_Cyton_SDK
//and CytonWifi class in OpenBCI-SuperCollider quark

n.sendMsg(\port, 57120);  //send to port
n.sendMsg(\latency, 5000);  //approx 100-15000


//simplified openbci data decoding - no checking for errors or dropped packages
(
var dataAction= {|num, data, accel| [num, data, accel].postln};  //edit and put your own code here
var uVScale= 4.5/24/(2**23-1)*1000000;
var accScale= 0.002/(2**4);
OSCdef(\data, {|msg|
	var buffer, byte, num, accel= #[0, 0, 0], data;
	(msg.size-1).do{|i|
		buffer= msg[i+1].collect{|x| if(x<0, {256+x}, {x})};
		byte= buffer[0];
		num= buffer[1];  //sample number
		data= Array.fill(8, {|i|  //eight channels of 24bit data converted to uV
			var v= (buffer[i*3+2]<<16)|(buffer[i*3+3]<<8)|buffer[i*3+4];
			if((v&0x800000)>0, {v|0xFF000000}, {v&0x00FFFFFF})*uVScale;
		});
		switch(byte,  //footer / stop byte
			0xC0, {  //accelerometer
				if(#[26, 27, 28, 29, 30, 31].any{|i| buffer[i]!=0}, {
					accel= Array.fill(3, {|i|  //three dimensions of 16bit data converted to g
						var v= (buffer[i*2+26]<<8)|buffer[i*2+27];
						if((v&0x8000)>0, {v|0xFFFF0000}, {v&0x0000FFFF})*accScale;
					});
				});
			};
		);
		dataAction.value(num, data, accel);
	};
}, \data, n);
)

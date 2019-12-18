// Declar objects
var app = require('http').createServer(handler),
  io = require('socket.io').listen(app),
  fs = require('fs'),
  url = require('url'),
  SerialPort = require('serialport'),

  // 'COM6' might need to be changed 
  sp = new SerialPort('COM5', {
    baudRate: 38400,
	  //parser: new serialPort.parsers.readline('\r\n') // might be changed too
  }),
  
  //PicMessage = '',
  
  readFile = function(pathname, res) {
    // an empty path returns index.html
    if (pathname === '/')
      pathname = 'index.html';

    fs.readFile( 'client/' + pathname, function(err, data) {
      if (err) {
        console.log(err);
        res.writeHead(500);
        return res.end('Error loading index.html');
      }
      res.writeHead(200);
      res.end(data);
    });
  };
  
  /*
  sendMessage = function(buffer) {
    PicMessage += buffer.toString();
    console.log(PicMessage);
    // detecting the end of the string
    if (PicMessage.indexOf('\r') >= 0) {
      // log the message into the terminal
       
      // send the message to the client
      //socket.volatile.emit('feedback', PicMessage);
      PicMessage = '';
    }
  };
	*/
	
	// creating a new websocket
io.sockets.on('connection', function(socket) {
	socket.on('message', function(data) {
		sp.write(data.toString()+'\r\n', function() {
		// console.log for debug purpose
		//console.log(data.toString());
		});
	});
});

//some listeners
sp.on('close', function(err) {
	console.log('COM6 Port closed!');
});

sp.on('error', function(err) {
	console.error('error', err);
});

sp.on('open', function() {
  // While opening the serial port, send the AT command to connect another bluetooth module
  sp.write("AT+CON78DB2F1405AA");
	console.log('Bluetooth connected');
  console.log('Serialport opened');
});
	
	
  // The server listens to the port 8086
app.listen(8086);
console.log('Website address: http://localhost:8086/');
	
function handler(req, res) {
  readFile(url.parse(req.url).pathname, res);
}
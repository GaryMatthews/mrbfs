static void mrbus_driver_serial()
{
	int fd, n;
	struct termios options;
	char buffer[256];
	char *bufptr;      // Current char in buffer 
	int  nbytes;       // Number of bytes read 
	fd_set         input;
	struct timeval timeout;

	fd = open(MODEMDEVICE, O_RDWR | O_NOCTTY | O_NDELAY); 
   if (fd < 0)
	{
		perror(MODEMDEVICE); exit(-1); 
	}
	fcntl(fd, F_SETFL, 0);

	FD_ZERO(&input);
	FD_SET(fd, &input);

	tcgetattr(fd,&options); // save current serial port settings 

	cfsetispeed(&options, B115200);
	cfsetospeed(&options, B115200);

   options.c_cflag &= ~CSIZE; // Mask the character size bits 
   options.c_cflag |= CS8;    // Select 8 data bits 

	options.c_cflag &= ~CRTSCTS;
	options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	options.c_iflag &= ~(IXON | IXOFF | IXANY);
	options.c_oflag &= ~OPOST;
	options.c_cc[VTIME] = 0;
   options.c_cc[VMIN]   = 1;   // blocking read until 5 chars received
   
   tcflush(fd, TCIFLUSH);
	tcsetattr(fd, TCSAFLUSH, &options);


	timeout.tv_sec  = 0;
	timeout.tv_usec = 500;
	n = select(fd, &input, NULL, NULL, &timeout);
   
	if (n > 0)
	{
      /* We have input */
      if (FD_ISSET(fd, &input))
		{
			while ((nbytes = read(fd, bufptr, buffer + sizeof(buffer) - bufptr - 1)) > 0)
			{
			
			
			}
		}
	}


	do
	{
		while ((nbytes = read(fd, bufptr, buffer + sizeof(buffer) - bufptr - 1)) > 0)
		{
		  bufptr += nbytes;
		  if (bufptr[-1] == '\n' || bufptr[-1] == '\r')
			      break;
		}

	} while(1);
}

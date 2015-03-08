#include "serial.h"

void listener_loop (serial *ser)
{
	int ii, size, in, start;
	int psize, dsize, ind;
	bool *ptr, loading;
	char *buff, *input, tag;
	float lasttime, currtime;
	struct timeval tv;
	packet *currpacket;

	start = 0;
	loading = false;
	input = new char [INPUT_BUFFER_SIZE];
	gettimeofday(&tv, NULL);
	currtime = (tv.tv_sec-1422700000) + tv.tv_usec*1e-6;
	lasttime = currtime;

	while (ser->listening)
	{
		while (ser->recv_queue_access) {}
		ser->recv_queue_access = true;
		// delete some if we have too many
		if (ser->recv_queue_packets.size() > 64)
		{
			cout << "WARNING: too many unclaimed packets!" << endl;
			for (ii=0; ii<ser->recv_queue_packets.size()-64; ii++)
			{
				delete ser->recv_queue_packets[ii];
				ser->recv_queue_packets.erase(ser->recv_queue_packets.begin());
			}
		}
		ser->recv_queue_access = false;
		// check for things to send
		if (ser->send_queue)
		{
			while(ser->send_queue_access) {};
			ser->send_queue_access = true;
			size = ser->send_queue_sizes[0];
			buff = ser->send_queue_buffers[0];
			ptr = ser->send_queue_confirm[0];
			ser->send_queue_buffers.erase(ser->send_queue_buffers.begin());
			ser->send_queue_sizes.erase(ser->send_queue_sizes.begin());
			ser->send_queue_confirm.erase(ser->send_queue_confirm.begin());
			ser->send_queue--;
			ser->send_queue_access = false;
			write(ser->fd, buff, size);
			if (ptr != NULL) *ptr = true;
		}

		// only check input if we've waited a bit
		gettimeofday(&tv, NULL);
		currtime = (tv.tv_sec-1422700000) + tv.tv_usec*1e-6;
		if (currtime - lasttime > 0.05)
		{
			// check for things to recv
			in = read(ser->fd, input, INPUT_BUFFER_SIZE);
			for (ii=0; ii<in; ii++)
			{
				// look for packet start signal
				if (input[ii]==0x11) start++;
				else start = max(0,start-1);
				if (start==3)
				{
					if (loading) // we were already loading a packet
					{
						cout << "WARNING: packet collision" << endl;
						delete currpacket;
						loading = false;
					}
					// grab rest of header
					if (ii+PACKET_HEADER_SIZE-3 < in)
					{
						memcpy(&psize, input+ii+PACKET_HEADER_PACKET_SIZE-2, sizeof(int));
						memcpy(&dsize, input+ii+PACKET_HEADER_DATA_SIZE-2, sizeof(int));
						memcpy(&tag, input+ii+PACKET_HEADER_TAG-2, sizeof(char));
						ii += PACKET_HEADER_SIZE-3;
						currpacket = new packet(dsize, tag, psize);
						loading = true;
						ind = 0;
						start = 0;
					} else {
						cout << "WARNING: packet miss" << endl; // TODO prevent this
					}
				}
				if (loading) { // load data into packet
					currpacket->buffer[ind+PACKET_HEADER_SIZE-1] = input[ii]; // TODO: change to memcpy
					ind ++;
					if (ind == dsize+1) // done loading
					{
						loading = false;
						// add to recv queue
						while (ser->recv_queue_access) {};
						ser->recv_queue_access = true;
						ser->recv_queue_packets.push_back(currpacket);
						ser->recv_queue_access = false;
					}
				}
			}
			lasttime = currtime;
		} else {
			usleep(100);
		}
	}

}


serial::serial ()
{
	initialized = false;
}


void serial::init (const char *portname)
{
	fd = open(portname, O_RDWR | O_NOCTTY | O_SYNC);
	if (fd < 0)
	{
		printf("HARD ERROR: %d opening %s: %s", errno, portname, strerror (errno));
		exit(-1);
	}
	set_interface_attribs (B230400, 0);
	set_blocking (0);

	send_queue = 0;
	send_queue_access = false;
	recv_queue_access = false;

	listening = true;
	listener = thread(listener_loop, this);
	initialized = true;
}

void serial::close ()
{
	listening = false;
	listener.join();
}

void serial::send (char *buffer, int size, bool *ptr)
{
	while (send_queue_access) {};
	send_queue_access = true;
	send_queue_buffers.push_back(buffer);
	send_queue_sizes.push_back(size);
	send_queue_confirm.push_back(ptr);
	send_queue++;
	send_queue_access = false;
}

packet* serial::recv (char tag, bool blocking)
{
	int ii, ind;
	bool found = false;
	packet *p;

	do
	{
		while (recv_queue_access) {};
		recv_queue_access = true;
		// look for oldest packet with matching tag
		for (ii=0; ii<recv_queue_packets.size(); ii++)
		{
			if (recv_queue_packets[ii]->getTag() == tag)
			{
				ind = ii;
				found = true;
				p = recv_queue_packets[ii];
				recv_queue_packets.erase(recv_queue_packets.begin()+ii);
				// jump to end of loop
				ii = recv_queue_packets.size();
			}
		}
		recv_queue_access = false;
		if (!found && blocking) usleep(1000);
	} while (!found && blocking);
	return p;
}

	int serial::set_interface_attribs (int speed, int parity)
	{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                printf ("error from tcgetattr", errno);
                return -1;
        }

        cfsetospeed (&tty, speed);
        cfsetispeed (&tty, speed);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
        // disable IGNBRK for mismatched speed tests; otherwise receive break
        // as \000 chars
        tty.c_iflag &= ~IGNBRK;         // ignore break signal
        tty.c_lflag = 0;                // no signaling chars, no echo,
                                        // no canonical processing
        tty.c_oflag = 0;                // no remapping, no delays
        tty.c_cc[VMIN]  = 0;            // read doesn't block
        tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

        tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

        tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
                                        // enable reading
        tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
        tty.c_cflag |= parity;
        tty.c_cflag &= ~CSTOPB;
        tty.c_cflag &= ~CRTSCTS;

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
        {
                printf ("error %d from tcsetattr", errno);
                return -1;
        }
        return 0;
	}

	void serial::set_blocking (int should_block)
	{
        struct termios tty;
        memset (&tty, 0, sizeof tty);
        if (tcgetattr (fd, &tty) != 0)
        {
                printf ("error %d from tggetattr", errno);
                return;
        }

        tty.c_cc[VMIN]  = should_block ? 1 : 0;
        tty.c_cc[VTIME] = 5;            	// 0.5 seconds read timeout

        if (tcsetattr (fd, TCSANOW, &tty) != 0)
                printf ("error %d setting term attributes", errno);
	}



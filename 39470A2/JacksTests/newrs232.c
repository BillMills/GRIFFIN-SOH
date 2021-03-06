/********************************************************************\

  Name:         rs232.c
  Created by:   Stefan Ritt

  Contents:     RS232 communication routines for MS-DOS, NT and Linux

  $Log: rs232.c,v $
  Revision 1.1  1999/12/20 10:18:11  midas
  Reorganized driver directory structure

  Revision 1.2  1998/10/12 12:18:58  midas
  Added Log tag in header


\********************************************************************/
#include <stdbool.h>
//#include "midas.h"

static bool debug_flag = false;  //CHANGED

#ifdef OS_MSDOS

#include <stdio.h>
#include <string.h>
#include <conio.h>
#include <bios.h>
#include <dos.h>
#include <stdlib.h>
#include <sys\timeb.h>
#include "rs232.h"

#define XON  0x11
#define XOFF 0x13
#define RING_BUFF_LEN 2000          /**** adjust if necessary  ****/
#define NUM_PORTS     1             /**** number of COMx ports ****/

static void interrupt com1int();
static void interrupt com2int();
static void comint_handler(int port);
static void rs232_exit(void);

extern void (*error)(char *str);

struct {
  unsigned int  port_base;
  unsigned int  vector;
  void interrupt (*new_intr)();
  void interrupt (*old_intr)();
  unsigned char initialized;
  char          ring_buff[RING_BUFF_LEN];
  unsigned char com_error, com_mode;
  int volatile  write_mark, read_mark, send_enable;
} ccb[] = {
  { 0x3F8, 0x0C, com1int },
  { 0x2F8, 0x0B, com2int }
};

/* ------------------------- rs232_init ----------------------------- */

void rs232_init(int port, int baud, int parity, int data_bit, int stop_bit,
              int mode)
/*********************************************************************
* Initialize serial port according to parameters. Install interrupt  *
* handler and uninstall it on exit.                                  *
* mode == 0: no handshake; 1: CTS/DSR; 2: XON/XOFF                   *
*********************************************************************/
{
#define CTS  0x10            /**** Bit masks for handshake lines ****/
#define DSR  0x20
#define RLSD 0x80

static int first_init  = 1;
int    parity_mask[]   = { 0x00, 0x08, 0x18 };
int    baud_list[]     = { 110, 150, 300, 600, 1200, 2400, 4800, 9600 };
int    i;

  for (i=0 ; i<8 ; i++)
    if (baud_list[i] == baud) break;

  /**** init com port ****/
  port--;
  bioscom(0, i*0x20 | parity_mask[parity] | (stop_bit-1)*4
             | (data_bit-5), port);

  /**** initialize ring buffer ****/
  ccb[port].write_mark  = 0;
  ccb[port].read_mark   = 0;
  ccb[port].com_error   = 0;
  ccb[port].com_mode    = mode;
  ccb[port].send_enable = 1;

  if (ccb[port].initialized)
    /**** reset old vector ****/
    setvect(ccb[port].vector,
            (void interrupt (far *) ()) ccb[port].old_intr);

  /**** save old vector, set new vector ****/
  ccb[port].old_intr =
      (void interrupt (far *) ())getvect(ccb[port].vector);
  setvect(ccb[port].vector,
      (void interrupt (far *) ())ccb[port].new_intr);

  ccb[port].initialized = 1;

  if (first_init)
    {
    first_init=0;
    atexit(rs232_exit);    /* call rs232_exit routine at exit of the program */
    }

  outportb(ccb[port].port_base+1,0x05);
			      /* enable Data Available Interrupt and     */
			      /* Recieve Line Status Interrupt           */
  outportb(ccb[port].port_base+4,0x0B); /* 8250 Interrupt enable         */

  inportb(ccb[port].port_base);         /* reset running interrupts      */
  inportb(ccb[port].port_base + 5);

  /**** 8259 Interrupt Controller init ****/
  outportb(0x21,inportb(0x21) & ~(0x10 >> port));
  outportb(0x20,0x20);        /* end of interrupt */
}

/* --------------------------- rs232_exit --------------------------- */

static void rs232_exit(void)
{
int i;

  /**** reset old vector ****/
  for (i=0 ; i<NUM_PORTS ; i++)
    if (ccb[i].initialized)
      {
      /**** disable interrupt controller ****/
      outportb(0x21,inportb(0x21) | (0x10 >> i));

      /**** 8250 interrupt disable ****/
      outportb(ccb[i].port_base + 4, 0);
      outportb(ccb[i].port_base + 1, 0);

      /**** reset old vector ****/
      setvect(ccb[i].vector, (void interrupt (far *) ()) ccb[i].old_intr);
      }
}

/* --------------------------- send char -------------------------- */

void rs232_putc(int port, char c)
/*********************************************************************
* Send a single character over the previously opened COM port        *
*********************************************************************/
{
char str[30];
int  retry;

#define TIMEOUT 3    /**** 3 seconds ****/

  port--;

  /**** wait until 8250 has sent previous character ****/
  while((inportb(ccb[port].port_base + 5) & 0x60) == 0);

  /**** wait until receiver ready (CTS and DSR high) or XON received ****/

  retry = 0;
  do
    {
    if ( (ccb[port].com_mode==1 &&
           (inportb(ccb[port].port_base + 6) & 0x30)!=0)
      || (ccb[port].com_mode==2 && ccb[port].send_enable)
      || (ccb[port].com_mode==0)) break;
    retry++;
    sleep(1); /**** wait maximal retry_max sec. ****/
    } while((retry < TIMEOUT));

  if (retry == TIMEOUT)
    {
    sprintf(str, "Timeout on COM%1d\n", port+1);
    error(str);
    }

  outportb(ccb[port].port_base,c);           /**** send char ****/
}

void computs(int port, char *str)
{
  while(*str)
    computc(port, *(str++));

  if (debug_flag)
    {
    FILE *f;

    f = fopen("rs232.log", "a");
    fprintf(f, "Out: %s\n", str);
    fclose(f);
    }
}

/* -------------------------- get string --------------------------- */

int rs232_gets(int port, char *s, int size, int end_char)
{
/*********************************************************************
* Copies the ringbuffer into the string s. Return the number of valid*
* chars in the string.                                               *
* end_char == 0 : copy the whole buffer                              *
* end_char != 0 : copy until "char" encountered                      *
*********************************************************************/
int  temp_write_mark, i=0, eos;
char str[30];

  port--;

  if (ccb[port].com_error > 0)
    {
    if ((ccb[port].com_error & 0x01) == 0x01)
      sprintf(str, "COM%1d: Ringbuffer overflow\n", port+1);
    if ((ccb[port].com_error & 0x04) == 0x04)
      sprintf(str, "COM%1d: Parity error\n", port+1);
    if ((ccb[port].com_error & 0x08) == 0x08)
      sprintf(str, "COM%1d: Missing stop-bit\n", port+1);
    if ((ccb[port].com_error & 0x10) == 0x10)
      sprintf(str, "COM%1d: Framing error\n", port+1);
    error(str);
    ccb[port].com_error = 0;  /**** reset error flag ****/
    }

  temp_write_mark = ccb[port].write_mark;
  if (temp_write_mark != ccb[port].read_mark)
    {
    temp_write_mark--;
    if (temp_write_mark < ccb[port].read_mark)
      temp_write_mark += RING_BUFF_LEN;

    /* if at least ten bytes room in the buffer, set DTR high / send XON */
    if (temp_write_mark - ccb[port].read_mark < RING_BUFF_LEN - 12)
      {
      if (ccb[port].com_mode == 2) computc(port, XON);
      if (ccb[port].com_mode == 1) outportb(ccb[port].port_base + 4,9);
      }

    if (end_char == 0)
      {
      for (i=0 ; (i<=temp_write_mark-ccb[port].read_mark) && (i<size-1) ; i++)
        s[i] = ccb[port].ring_buff[(i+ccb[port].read_mark) % RING_BUFF_LEN];
      ccb[port].read_mark = (ccb[port].read_mark + i) % RING_BUFF_LEN;
      }
    else
      {
      eos=-1;
      for (i=0 ; (i<=temp_write_mark-ccb[port].read_mark) && (i<size-1) ; i++)
        if (ccb[port].ring_buff[(i+ccb[port].read_mark) % RING_BUFF_LEN]
              == end_char)
          {
          eos = i;
          break;
          }
      if (eos>=0)
        for (i=0 ; i<=eos ; i++)
          s[i] = ccb[port].ring_buff[(i+ccb[port].read_mark) % RING_BUFF_LEN];
      ccb[port].read_mark = (ccb[port].read_mark + i) % RING_BUFF_LEN;
      }
    }
  s[i]=0;

  if (debug_flag)
    {
    FILE *f;

    f = fopen("rs232.log", "a");
    fprintf(f, "In: %s\n", s);
    fclose(f);
    }
  
  return(i);
}

/* ----------------------- wait for a string ----------------------- */

int rs232_waitfor(int port, char *str, char *retstr, int size, int seconds)
{
/*********************************************************************
* Wait for a specific string. Timeout after "seconds". Does NOT re-  *
* move the string from the ringbuffer.                               *
*********************************************************************/
int          temp_write_mark, i, j, k;
char         errstr[80];
struct timeb start_time, act_time;

  port--;
  if (ccb[port].com_error > 0)
    {
    if ((ccb[port].com_error & 0x01) == 0x01)
      sprintf(errstr, "COM%1d: Ringbuffer overflow\n", port+1);
    if ((ccb[port].com_error & 0x04) == 0x04)
      sprintf(errstr, "COM%1d: Parity error\n", port+1);
    if ((ccb[port].com_error & 0x08) == 0x08)
      sprintf(errstr, "COM%1d: Missing stop-bit\n", port+1);
    if ((ccb[port].com_error & 0x10) == 0x10)
      sprintf(errstr, "COM%1d: Framing error\n", port+1);
    error(errstr);
    ccb[port].com_error = 0;  /**** reset error flag ****/
    }

  ftime(&start_time);

  do
    {
    temp_write_mark = ccb[port].write_mark;
    if (temp_write_mark != ccb[port].read_mark)
      {
      temp_write_mark--;
      if (temp_write_mark < ccb[port].read_mark)
        temp_write_mark += RING_BUFF_LEN;

      /* if at least ten bytes room in the buffer, set DTR high / send XON */
      if (temp_write_mark - ccb[port].read_mark < RING_BUFF_LEN - 12)
        {
        if (ccb[port].com_mode == 2) computc(port, XON);
        if (ccb[port].com_mode == 1) outportb(ccb[port].port_base + 4,9);
        }

      if (temp_write_mark - ccb[port].read_mark + 1 >= strlen(str))
        {
        for (i=0 ; i <= temp_write_mark-ccb[port].read_mark-strlen(str)+1 ; i++)
          {
          if (str[0] == ccb[port].ring_buff[(i+ccb[port].read_mark) % RING_BUFF_LEN])
            {
            for (j=1 ; j<strlen(str) ; j++)
              if (str[j] != ccb[port].ring_buff[(i+j+ccb[port].read_mark) % RING_BUFF_LEN])
                break;
            if (j == strlen(str))
              {
              for (k=0 ; k<=i+j-1 && k<size-1 ; k++)
                retstr[k] =
                  ccb[port].ring_buff[(k+ccb[port].read_mark) % RING_BUFF_LEN];
              ccb[port].read_mark = (ccb[port].read_mark + k) % RING_BUFF_LEN;
              retstr[k] = 0;

              if (debug_flag)
                {
                FILE *f;

                f = fopen("rs232.log", "a");
                fprintf(f, "Waitfor: %s\n", retstr);
                fclose(f);
                }

              return(k);
              }
            }
          }
        }
      }
    ftime(&act_time);
    } while( (double) act_time.time + act_time.millitm/1000 -
                      start_time.time - start_time.millitm/1000 < seconds);

  sprintf(errstr, "Timeout WAITFOR (%s)", str);
  error(errstr);

  retstr[0] = 0;
  return(0);
}

/* -------------------------- Interrupt Handler -------------------------- */

void interrupt com1int()
{
  comint_handler(1);
}

void interrupt com2int()
{
  comint_handler(2);
}

void comint_handler(int port)
{
/*********************************************************************
* This is the seial interrupt handler. It is called when a char is   *
* received or an serial error occured.                               *
*********************************************************************/
char c;
unsigned char int_type;
int old_write_mark;

  port--;
  while ( ((int_type = inportb(ccb[port].port_base+2)) & 1) == 0 )
  /**** as long as an interrupt is pending ****/
    {
    if (int_type == 6)              /* Receiver Line Status Error   */
      ccb[port].com_error = inportb(ccb[port].port_base + 5) & 0x1E;

    if (int_type == 4)              /* Received Data available      */
      {
      c = inportb(ccb[port].port_base);
      if (c == XON) ccb[port].send_enable = 1;
      else if (c == XOFF) ccb[port].send_enable = 0;
      else
        {
        /**** write received char into the ringbuffer ****/
        ccb[port].ring_buff[ccb[port].write_mark] = c;

old_write_mark = ccb[port].write_mark;

        ccb[port].write_mark = (ccb[port].write_mark + 1) % RING_BUFF_LEN;

        /**** if ringbuffer is nearly full, set DTR low / send XOFF ****/
        if (((ccb[port].write_mark + 1) % RING_BUFF_LEN) == ccb[port].read_mark)
          {
          if (ccb[port].com_mode == 2) computc(port, XOFF);
            else outportb(ccb[port].port_base + 4,8);
          }

        /**** if ringbuffer if full, shift endmark and set error ****/
        if (ccb[port].write_mark == ccb[port].read_mark)
          {
          ccb[port].com_error = 1;
cprintf("old_write_mark: %d, write_mark: %d, read_mark: %d\n\r",
         old_write_mark, ccb[port].write_mark, ccb[port].read_mark);

          ccb[port].read_mark = (ccb[port].read_mark + 1) % RING_BUFF_LEN;
          }
        }
      }
    }   /* end-of-while */

  outportb(0x20,0x20);         /* end of interrupt */
}

#endif /* OS_MSDOS */

/*---- Windows NT ------------------------------------------------------------*/

#ifdef OS_WINNT

#include <stdio.h>

int rs232_init(char *port, int baud, char parity, int data_bit, int stop_bit,
              int mode)
{
char          str[80];
DCB           dcb ;
HANDLE        hDev;
COMMTIMEOUTS  CommTimeOuts;

  sprintf(str, "\\\\.\\%s", port);

  hDev = CreateFile( str, GENERIC_READ | GENERIC_WRITE,
                  0,                    // exclusive access
                  NULL,                 // no security attrs
                  OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | 
                  0,
                  NULL );
                  
  if (hDev == (HANDLE) -1)
    return (int) hDev;

  GetCommState( hDev, &dcb ) ;

  sprintf(str, "baud=%d parity=%c data=%d stop=%d", baud, parity, data_bit, stop_bit);
  BuildCommDCB(str, &dcb);

  if (SetCommState( hDev, &dcb ) == FALSE)
    return -1;

  // SetCommMask( hDev, EV_RXCHAR );
  SetupComm( hDev, 4096, 4096 );

  CommTimeOuts.ReadIntervalTimeout = 0 ;
  CommTimeOuts.ReadTotalTimeoutMultiplier = 0 ;
  CommTimeOuts.ReadTotalTimeoutConstant = 0 ;
  CommTimeOuts.WriteTotalTimeoutMultiplier = 0 ;
  CommTimeOuts.WriteTotalTimeoutConstant = 5000 ;
  
  SetCommTimeouts( (HANDLE) hDev, &CommTimeOuts ) ;

  return (int) hDev;
}

/*----------------------------------------------------------------------------*/

void rs232_exit(int hDev)
{
  EscapeCommFunction( (HANDLE) hDev, CLRDTR ) ;

  PurgeComm( (HANDLE) hDev, PURGE_TXABORT | PURGE_RXABORT |
                                  PURGE_TXCLEAR | PURGE_RXCLEAR ) ;
  CloseHandle( (HANDLE) hDev );
}

/*----------------------------------------------------------------------------*/

void rs232_puts(int hDev, char *str)
{
DWORD written;
 
  if (debug_flag)
    {
    FILE *f;

    f = fopen("rs232.log", "a");
    fprintf(f, "Out: %s\n", str);
    fclose(f);
    }

  WriteFile( (HANDLE) hDev, str, strlen(str), &written, NULL);
}

/*----------------------------------------------------------------------------*/

int rs232_gets(int hDev, char *str, int size, int end_char)
{
int           i, status;
DWORD         read;
COMMTIMEOUTS  CommTimeOuts;

  GetCommTimeouts( (HANDLE) hDev, &CommTimeOuts ) ;

  CommTimeOuts.ReadIntervalTimeout = 0 ;
  CommTimeOuts.ReadTotalTimeoutMultiplier = 0 ;
  CommTimeOuts.ReadTotalTimeoutConstant = 500 ;
  
  SetCommTimeouts( (HANDLE) hDev, &CommTimeOuts ) ;

  memset(str, 0, size);
  for (i=0 ; ; i++)
    {
    status = ReadFile( (HANDLE) hDev, str+i, 1, &read, NULL);
    if (!status || read == 0)
      return 0;

    if (str[i] == end_char)
      break;
    }

  if (debug_flag)
    {
    FILE *f;

    f = fopen("rs232.log", "a");
    fprintf(f, "In: %s\n", str);
    fclose(f);
    }

  return i;
}

/*----------------------------------------------------------------------------*/

int rs232_waitfor(int hDev, char *str, char *retstr, int size, int seconds)
{
int           i, status;
DWORD         read;
COMMTIMEOUTS  CommTimeOuts;

  GetCommTimeouts( (HANDLE) hDev, &CommTimeOuts ) ;

  CommTimeOuts.ReadIntervalTimeout = 0 ;
  CommTimeOuts.ReadTotalTimeoutMultiplier = 0 ;
  CommTimeOuts.ReadTotalTimeoutConstant = 1000*seconds ;
  
  SetCommTimeouts( (HANDLE) hDev, &CommTimeOuts ) ;

  memset(retstr, 0, size);
  for (i=0 ; i<size-1 ; i++)
    {
    status = ReadFile( (HANDLE) hDev, retstr+i, 1, &read, NULL);
    if (!status || read == 0)
      return 0;

    if (strstr(retstr, str) != NULL)
      break;
    }
  
  if (debug_flag)
    {
    FILE *f;

    f = fopen("rs232.log", "a");
    fprintf(f, "Waitfor: %s\n", retstr);
    fclose(f);
    }

  return i+1;
}

#endif  /* End of OS_WINNT */

/*----------------------------------------------------------------------------*/
#ifdef OS_LINUX

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>

#include "lxrs232.h"


/*----------------------------------------------------------------------------*/
int rs232_init(int port, int baud, char parity, int data_bit, int stop_bit,
              int mode)
{
  char          str[80];
  int           i=-1, hDev, status;
  struct termios termios_p;
  speed_t speed;
  
  struct {
      int speed;
      int code;
  } Baud[10] = {
    {300, 7}, {600,10}, {1200,11},
    {1800,12}, {2400,13}, {4800,14}, {9600,15},
    {19200,16}, {38400,17}, {0,0}
  };

  /* check baud argument */
  while (Baud[++i].speed)
  {
    if (baud == Baud[i].speed)
      break;
  }
  if (Baud[i].speed == 0)
  {
    cm_msg(MERROR,"rs232_init","Unvalid baud speed (%d)",baud);
    return 0;
  }
  baud = Baud[i].code;
  
  sprintf(str, "/dev/ttyS%d", port);


  hDev = open (str, O_RDWR | O_NONBLOCK);
  if (hDev < 0)
  {
    cm_msg(MERROR,"rs232_init","Can't open serial port %s",str);
    hDev = 0;
    return hDev;
  }

  /* Port setup */
  status =  tcgetattr( (int) hDev, &termios_p);
  /* input */
  termios_p.c_iflag &= ~(ICRNL|IXON);
  termios_p.c_iflag |= IGNBRK;

  /* output */
  termios_p.c_oflag &= ~(OPOST|ONLCR);
  
  /* control */
  termios_p.c_cflag &= ~(CRTSCTS | CSIZE);
  termios_p.c_cflag |= (CS8);
  
  /* l flags */
  termios_p.c_lflag &= ~(ISIG|ICANON|IEXTEN|ECHO|ECHOE|ECHOK|ECHOKE|ECHOCTL);
  
  status =  tcsetattr( (int) hDev, TCSANOW, &termios_p);

/*
  speed =  cfgetispeed (&termios_p);
  printf("getispeed status =%d, ospeed:%d \n", status, speed);
  speed =  cfgetospeed (&termios_p);
  printf("getospeed status =%d, ispeed:%d \n", status, speed);
*/
  /* set BAUD speed */
//  status =  cfsetispeed(&termios_p, (speed_t) baud);
//  printf(" setispeed status =%d errno %s\n", status, strerror(errno));
//  status =  cfsetospeed(&termios_p, (speed_t) baud);
//  printf(" setospeed status =%d errno %s\n", status, strerror(errno));
 
//  status =  tcsetattr( (int) hDev, TCSANOW, &termios_p);
//  printf(" tcsetattr status =%d errno %s\n", status, strerror(errno));
/*
  speed =  cfgetispeed (&termios_p);
  printf("getispeed status =%d, ospeed:%d \n", status, speed);
  speed =  cfgetospeed (&termios_p);
  printf("getospeed status =%d, ispeed:%d \n", status, speed);
*/
  printf("Handle:%d port:%s baud=%d parity=%c data=%d stop=%d\n"
	 ,hDev, str, Baud[i].speed,
	 parity,data_bit,stop_bit);
  return (int) hDev;
}

/*----------------------------------------------------------------------------*/
/** rs232_exit
    \begin{description}
    \item[Description:] Close connection to serial port
    \item[Remarks:] 
    \end{description}
    @memo Close connection to serial port.
    @param hDev file description
    @return void
*/
void rs232_exit(int hDev)
{
  close (hDev);
}

/*----------------------------------------------------------------------------*/
/** rs232_puts
    \begin{description}
    \item[Description:] Writes a string to the serial port.
    \item[Remarks:] 
    \end{description}
    @memo Write to serial port.
    @param hDev file description
    @param str  string to send to serial port
    @return void
*/
void rs232_puts(int hDev, char *str)
{
  INT   nbytes;
  DWORD written;
  char *t;

  if (debug_flag)
  {
    FILE *f;
    
    f = fopen("rs232.log", "a");
    fprintf(f, "Out: %s\n", str);
    fclose(f);
  }

  nbytes = write(hDev, str, strlen(str)); 
  if (nbytes < 1)
  {
    cm_msg(MINFO,"rs232_puts","Failed to write entire str (%d)", nbytes);
  }
}

/*----------------------------------------------------------------------------*/
/** rs232_gets_nowait
    \begin{description}
    \item[Description:] Reads from the serial port without blocking.
    \item[Remarks:] 
    \end{description}
    @memo None blocking read from serial port.
    @param hDev file description
    @param str  receiving buffer
    @param size maximum size of receiving buffer (str)
    @param end_char end of string marker
    @return number of bytes received
*/
int rs232_gets_nowait(int hDev, char *str, int size, int end_char)
{
  int       i, status, nbytes;
  DWORD     read;
  int fd    = hDev;
  int flags = fcntl( fd, F_GETFL );
  
  // Turn on the non-blocking IO flag for the port
  fprintf(stderr,"no_wait called\nInitially flags = 0x%08x\n",flags);
  if (fcntl( fd, F_SETFL, (flags | O_NONBLOCK | O_NDELAY) ) < 0)
  {
    cm_msg(MINFO,"rs232_gets","fnctl(): Failed to set non-blocking IO");
    return 0;
  }
  fprintf(stderr,"FYI:  O_NONBLOCK|O_NDELAY = 0x%08x\n",O_NONBLOCK|O_NDELAY);
  flags = fcntl(fd,F_GETFL);
  fprintf(stderr,"about to call wait with flags=0x%08x\n",flags);
  nbytes = rs232_gets_wait(fd, str, size, end_char);  
  // Reset the non-blocking status

  if (fcntl( fd, F_SETFL, (flags & ~(O_NONBLOCK | O_NDELAY)) ) < 0)
  {
    cm_msg(MINFO,"rs232_gets","fnctl(): Failed to reset non-blocking IO");
    return 0;
  }
  
  if (debug_flag)
  {
    FILE *f;
    
    f = fopen("rs232.log", "a");
    fprintf(f, "In: %s\n", str);
    fclose(f);
  }
  return nbytes;
}

/*----------------------------------------------------------------------------*/
/** rs232_gets_wait
    \begin{description}
    \item[Description:] Blocking read from the serial port.
    \item[Remarks:] 
    \end{description}
    @memo Blocking read from serial port.
    @param hDev file description
    @param str  receiving buffer
    @param size maximum size of receiving buffer (str)
    @param end_char end of string marker
    @return number of bytes received
*/
int rs232_gets_wait(int hDev, char *str, int size, int end_char)
{
  int    i, status, nbytes, ntmp;
  char   tmp[16], *t, *cr;
  int flags;
  int fd=hDev;
  flags = fcntl(fd, F_GETFL);
  fprintf(stderr,"Entered rs232_gets_wait with flags=0x%08x\n",flags);
  memset(str,0,size);
  /* The read may be incomplete, so keep reading until end_char found
     if no end_char in trouble for now */
  t = str;
  for (i=0;;i++)
  {
    nbytes = read(hDev, t, size-1);
    t  += nbytes;
    if (nbytes < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
	nbytes = 0;
      else
      {
	cm_msg(MINFO,"rs232_gets_wait","fnctl(): read error %d", nbytes);
	return 0;
      }
    }
    /* debugging, shows the contruction of the string done
       by several read */
    /* printf("\ni:%d |%s|",i,  str); */

    /* detection of the termination end_char
       in this case remove the '\rend_char' from the end */
    if (cr = strchr(str , end_char)) {
      *(cr-1) = 0;
      break;
    }
  }
  
  if (debug_flag)
  {
    FILE *f;
    
    f = fopen("rs232.log", "a");
    fprintf(f, "In: %s\n", str);
    fclose(f);
  }

  /* return the true string length (zeor terminate) */
  return strlen(str);
}

/*----------------------------------------------------------------------------*/
/** rs232_waitfor
    \begin{description}
    \item[Description:] Blocking read until requested response has been received.
    \item[Remarks:] 
    \end{description}
    @memo Blocking read until response received.
    @param hDev file description
    @param str  receiving buffer
    @param retstr  string to compare
    @param size maximum size of receiving buffer (str)
    @param seconds timeout in seconds
    @return number of bytes received or 0 for no match or timeout
*/
int rs232_waitfor(int hDev, char *str, char *retstr, int size, int seconds)
{
  int           i, status;
  DWORD         nbytes;

  memset(retstr, 0, size);

  nbytes = rs232_gets_wait(hDev, str, size, 0);
  if (nbytes > 0)
  {
    if (strstr(retstr, str) != NULL)
      return 0;
  }

  return nbytes;
  
  if (debug_flag)
  {
    FILE *f;
    
    f = fopen("rs232.log", "a");
    fprintf(f, "Waitfor: %s\n", retstr);
    fclose(f);
  }
}

#endif /* End of OS_LINUX */

/*----------------------------------------------------------------------------*/
void rs232_debug(bool flag)
{
  debug_flag = flag;
}

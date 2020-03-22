/*------------------------------------------
             SIMPLE SMTP CLIENT
           CS375 Computer Networks
                  Proj1b.c
         Desmond Liang & Peize Song
--------------------------------------------*/

#include <stdio.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <string.h>


#define PORT 25
#define BUFFER 1024
#define SMTP_IP "140.141.2.43" //mail.denison.edu
#define COLOR_GREEN   "\x1b[32m"
#define COLOR_RED     "\x1b[31m"
#define COLOR_RESET   "\x1b[0m"
#define COLOR_YELLOW   "\x1b[33m"

/*------------------------------------------
  writeHR
  writes a seperation line for aesthetics
--------------------------------------------*/
void writeHR(){
  printf("----------------------------------------------------------\n");
}

/*------------------------------------------
  sendLine
  sends a given line to a given socket
  takes a socket and a words as parameters
  returns the number of bytes sent,
  returns -1 if failed the send.
--------------------------------------------*/
int sendLine(int sock, char *words){
  return send(sock, words, strlen(words), 0);
}

/*------------------------------------------
  checkResponse
  retrieve the server response and check
  the status code.
  takes a socket & expected code as parameters
  returns 0 if good, 1 if bad
--------------------------------------------*/
int checkResponse(int sock, char *codes){
  char response[BUFFER] = {0};
  recv(sock, response, 44, 0);
  //printf("%s\n",response);
  if(response[0]==codes[0]&&
     response[1]==codes[1]&&
     response[2]==codes[2]){
    return 0;
  } else {
    return 1;
  }
}

/*------------------------------------------
  HELO
  sends HELO to the socket
  takes a socket as a parameter
  returns 0 if good, 1 if bad
--------------------------------------------*/
int HELO(int sock){
  sendLine(sock, "HELO localhost\r\n");
  if(checkResponse(sock, "250")==1){
    printf(COLOR_RED" × HELO failed.\n"COLOR_RESET);
    return 1;
  } else {
    return 0;
  }

}

/*------------------------------------------
  MAILFROM
  sets MAIL FROM for the smtp server
  takes a socket and a mail from addr as params
  returns 0 if good, 1 if bad
--------------------------------------------*/
int MAILFROM(int sock, char *login){
  char *buf[BUFFER];
  snprintf(buf, BUFFER, "MAIL FROM:<%s>\r\n", login);
  sendLine(sock, buf);
  if(checkResponse(sock, "250")==1){
    printf(COLOR_RED" × MAIL FROM failed.\n"COLOR_RESET);
    return 1;
  } else {
    printf(COLOR_GREEN" √ Your email is recorded.\n"COLOR_RESET);
    return 0;
  }
}

/*------------------------------------------
  RCPTTO
  collect recipients for the email
  takes a socket as a parameter
  returns 0 if all set, 1 if bad
--------------------------------------------*/
int RCPTTO(int sock){
  char *rcpt[BUFFER], *buf[BUFFER], *Y[BUFFER];
  while(1){
    printf("Please enter a recipient's email: ");
    scanf("%s", rcpt);
    snprintf(buf, BUFFER, "RCPT TO:<%s>\r\n", rcpt);
    sendLine(sock, buf);
    if(checkResponse(sock, "250")==1){
      printf(COLOR_RED" × Adding recipient failed.\n"COLOR_RESET);
      return 1;
    } else {
      printf(COLOR_GREEN" √ Recipient added.\n"COLOR_RESET);
    }
    writeHR();
    printf(COLOR_YELLOW"Type anything to continue adding
            recipients, return to skip. "COLOR_RESET);
    scanf("%c", Y);
    printf("%c", Y);

    fgets(Y, BUFFER, stdin);
    if(strlen(Y)<=1){ // empty line
      break;
    } else {
      printf("\rGo on, ");
    }
  }
  return 0;
}

/*------------------------------------------
  DATA
  composes the email
  takes a socket and a mail from addr as params
  returns 0 if good, 1 if bad
--------------------------------------------*/
int DATA(int sock, char *login){
  sendLine(sock, "DATA\r\n");
  if(checkResponse(sock, "354")==1){
    printf(COLOR_RED" × DATA signal failed.\n"COLOR_RESET);
    return 1;
  }

  char *buf[BUFFER], *input[BUFFER];

  printf("Enter your name: ");
  scanf("%s", input);
  snprintf(buf, BUFFER, "From: %s <%s>\r\n", input, login);
  sendLine(sock, buf);
  printf(COLOR_GREEN" √ Name added.\n"COLOR_RESET);
  writeHR();

  printf("Enter email subject: ");
  scanf("%s", input);
  snprintf(buf, BUFFER, "Subject: %s \r\n\r\n", input);
  sendLine(sock, buf);
  printf(COLOR_GREEN" √ Subject line added.\n"COLOR_RESET);
  writeHR();

  int sendFlag = 0;
  printf("Begin composing your email, Press return twice to send. \n");
  writeHR();
  while(sendFlag < 2){
    fgets(input, BUFFER, stdin);
    if(strlen(input)<=1){ // empty line
      sendFlag += 1;
    } else {
      sendFlag = 0;
    }
    sendLine(sock, input);
  }

  sendLine(sock, "\r\n.\r\n");
  if(checkResponse(sock, "250")==1){
    printf("%s\n","MAIL SENT error.");
    return 1;
  } else {
    writeHR();
    printf(COLOR_GREEN" √ Congratulations, email is sent.\n"COLOR_RESET);
  }

  return 0;
}


/*------------------------------------------
  smtp
  performs a standard SMTP protocol on a
  designated server.
  returns 0 when done.
--------------------------------------------*/
int smtp(){
  char *login[BUFFER];
  printf(COLOR_GREEN "   Simple SMTP Client\n" COLOR_RESET);

  // create socket for the connection
  struct sockaddr_in address;
  int sock;
  struct sockaddr_in serv_addr;

  if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0){
      printf("\n Socket creation error \n");
      return -1;
  }

  memset(&serv_addr, '0', sizeof(serv_addr));

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = htons(PORT);

  inet_pton(AF_INET, SMTP_IP, &serv_addr.sin_addr);

  // connect to server
  connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
  checkResponse(sock,"220");



  // performs SMTP protocol talk
  writeHR();
  HELO(sock);

  writeHR();
  printf("Please enter your email: ");
  scanf("%s", login);
  if(MAILFROM(sock,login)==1){
    return 0;
  }

  writeHR();
  RCPTTO(sock);

  writeHR();
  DATA(sock,login);
  close(sock);

  writeHR();
  return 0;
}

void main(int argc, char const *argv[])
{
  smtp();
}

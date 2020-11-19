1. You have to use make to compile the software
  $ make
gcc -Wall -Werror -g -o oss oss.c -lpthread
gcc -Wall -Werror -g -o user_proc user_proc.c -lpthread

2. Run the program
  $ ./oss -v > output.txt

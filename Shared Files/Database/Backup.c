#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

/* 
   script to enable periodic backups of the database
   usage for normal users: ./Backup Database.db
*/
int main(int argc, char **argv)
{
	char str[1024];
	unsigned t;
	printf("The database backup tool is now running.\n");
	
	while(1)
	{
		t = (unsigned)time(NULL);
		if (t % 10 == 0)
		{
			sprintf(str, "cp %s /root/backups/b%u.db",
					argv[1], t);
			setuid(0);
			system(str);
			sleep(2);
			printf("Saved: /root/backups/b%u.db\n", t);
		}
		sleep(1);
	}
}
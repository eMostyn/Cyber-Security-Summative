#include <stdio.h>
#include <string.h>

int main(void)
{
	int correct=0;
	char buff[64];
	
	printf("====================================================\n");
	printf("Welcome to the Durhamazon business accounts.\n");
	printf("This tool stores the private company bitcoin wallet.\n");
	printf("Only the CEO is allowed to login.\n");
	printf("====================================================\n");
	printf("Please enter the password:\n");

	gets(buff);
	
	if (strcmp(buff, "<HIDDEN>"))
		printf("Incorrect password\n");
	else
		correct=1;
	
	if (correct)
	{
		printf("Correct password\n");
		printf("Your private key is: <HIDDEN>\n");
	}
	
	return 0;
}
#include <stdio.h>
#include <syslog.h>
#include <string.h>

int main(int argc, char *argv[])
{
	openlog(NULL, 0, LOG_USER);
	
	if (argc != 2)
        {
                syslog(LOG_ERR, "Incorrect number of arguments!");
                return 1;
        }


	FILE *test = fopen(argv[1], "w+");

	if (!test)
	{
		syslog(LOG_ERR, "Error opening file");
		return 1;
	}
	else
	{
		fprintf(test, argv[2]);
		syslog(LOG_DEBUG, "Writing %s to %s\n", argv[2], argv[1]);
	}

	return 0;
}

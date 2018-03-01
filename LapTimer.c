#include<avr/io.h>
#include<avr/interrupt.h>
#include<stdint.h>
#include<stdlib.h>
#include<util/atomic.h>

#define F_CPU 4000000   // Define CPU speed
#define tick F_CPU/64   // Number of ticks (cycles) in 1 second, Define of how long the timer/counter, counts in one second, with prescaler sat to 64

typedef struct // Struct for storing time
{
	volatile uint8_t min;
	volatile uint8_t sec;
	volatile uint8_t hSec;
}	time_type;

time_type clock = {0, 0, 0};     // Global variable to store the current time

typedef struct // Struct for storing runners id and their time
{
	uint8_t id;
	uint8_t running;	// Boolean to check whether the runner is currently running or finished
	time_type time;		// If running is set to true, time is the start time, else it is the lap time
}	runner_type;

typedef struct node {	// Struct for linked list for runner_type
	runner_type runner;
	struct node* prev;	// Pointer to previous element in list, NULL if it's the first element
	struct node* next;	// Pointer to next element in list, NULL if it's the last element
} runner_node;

runner_node* runnerListHead;	// Global variable for storing the head of the linked list of runners

// prototype of local functions
static void initTimer(void);
static void initPort(void);
static void stopTimer(void);
static void startTimer(void);
static void incClock(time_type *time_p);
static runner_node* addRunner(runner_node * head, runner_type runner);
static void inputController(void);
static time_type averageLapTime(void);
static runner_node* sortedByTime(runner_node* head);
static void freeRunnerList(runner_node* head);
static void outputBest(void);
static uint8_t getTimeInSec(runner_type runner);

/*
*	ISR(interrupt vector)
*
*	Interrupt service routines
*	Timer 1 interrupts once a second, the clock is then incremented by 1 second
*/
ISR(TIMER1_COMPA_vect)
{
	incClock(&clock);	// Increments the clock, the clock pointer refers to the global clock variable storing the current time
}

/*
*  void initTimer(void)
*
*  Timer is initialized and
*  Global interrupt is enabled.
*/
static void initTimer(void)
{
	TCCR1B = (1 << WGM12);      // Configure timer 1 for CTC mode
	TCNT1  = 0;                 // Set Timer Counter 1 = 0,
	OCR1A  = tick;              // Output Compare Register A
	TIMSK  = (1 << OCIE1A);     // Enable interrupt on OCR1A match
	sei();                      // Enable global interrupt
}

/*
*	void initPort(void)
*
*	Initialize ports to output and inputs
*/
static void initPort(void)
{
	DDRA = 0xff;  // Output best runner id (8 bit)
	DDRD = 0;	  // Handshake signals input (bit 0 - clock start/stop, bit 4 - runner start/finish)
	DDRE = 0;	  // Input runner id
}

/*
*  void stopTimer(void)
*
*  Stops timer 1
*/
static void stopTimer(void)
{
	TCCR1B &= ~((1 << CS10 ) | (1 << CS11 )) ; // Clear bit CS10 and CS11 in TCCR1B register  (TCCRIB = TCCR1B AND not( 0000 0001 OR 0000 0010))
}

/*
*  void startTimer(void)
*
*  Starts timer 1
*/
static void startTimer(void)
{
   TCCR1B |= ((1 << CS10 ) | (1 << CS11 )) ; // Set bit CS10 and CS11 in TCCR1B register (TCCRIB = TCCR1B OR ( 0000 0001 OR 0000 0010))
}

/*
*  void incClock(time_type *time_p)
*
*  Increments the time stored in time_type *time_p by 1 second
*
*	in:  *time_p :  A pointer to the time_type struct that stores the current time
*	out:  void   :  This function has no output variable!
*/
static void incClock(time_type *time_p)
{
	if (time_p -> sec < 59)
	     (time_p -> sec)++;
	else
	{
		(time_p -> sec) = 0;
		(time_p -> min)++;
	}

	time_p -> hSec = TCNT1;		// Hundredth of seconds is retrieved from the timer 1 counter, since it stores the time since last interrupt i.e. last second
}

/*
*	runner_node* addRunner(runner_node * head, runner_type runner)
*
*	Adds the runner at the end of the linked list starting at head
*
*	in: runner_node * head - pointer to start of the linked list
*		runner_type runner - the runner to be added to the list
*	out: runner_type* - Pointer to the newly added runner_node
*/
static runner_node* addRunner(runner_node* head, runner_type runner)
{
	static runner_node* prev = NULL;	// Static variable for storing the previous runner_node each time the function is called
	runner_node* current = head;		// Set the current selected node to head i.e. first element of the list

	while (current -> next != NULL) {	// Iterate to the end of the linked list
		current = current -> next;		// Move to next node each iteration
	}

	current -> next = malloc(sizeof(runner_node));	// Allocate space on the heap for the new runner
	current -> runner = runner;
	current -> prev = prev;
	current -> next = NULL;

	prev = current;

	return current;
}

/*
*	void inputController(void)
*
*	Controls the program. Registers inputs, time and manages outputs
*/
static void inputController(void)
{
	static uint8_t clockRunning = 0;
	static runner_type currentRunner;	// Variable for temporary storing the runner currently running

	time_type averageTime = {0, 0};		// Empty time_type for storing the average lap time of all the runners
	runner_node* sortedListHead;		// Pointer to the head of the sorted list of runners

	/* Port D, bit 0 - Start/stop timer */
	if (PIND & 1)		// if bit 0 is set
	{
		if (clockRunning)// Check if the clock is running
		{
			clockRunning = 0;
			stopTimer();

			averageTime = averageLapTime();
			sortedListHead = sortedByTime(runnerListHead);
			return;
		}
		else
		{
			startTimer();
			clockRunning = 1;
		}
	}

	/* Runner starts/finish running (Port B pin 4) */
	if (PIND & (1 << 4) && clockRunning)
	{
		if (currentRunner.running){
			time_type lapTime;

			// Register finish time, save delta
			ATOMIC_BLOCK(ATOMIC_RESTORESTATE)	// Temporary turn off interrupts while reading time to prevent clock from incrementing while reading
			{
				lapTime.min = clock.min - currentRunner.time.min;
				lapTime.sec = clock.sec - currentRunner.time.sec;
				lapTime.hSec = TCNT1 - currentRunner.time.hSec;
			}

			currentRunner.time = lapTime;	// Set the time difference as the new time for currentRunner
			currentRunner.running = 0;		// Current runner is no longer running
		}
		else
		{
			currentRunner.running = 1;			// The runner is starting to run, the currentRunner's time is the start time
			currentRunner.id = PINE;			// Read id from port E and save

			// Register start time
			ATOMIC_BLOCK(ATOMIC_RESTORESTATE)	// Temporary turn off interrupts while reading time
			{
				currentRunner.time = clock;
				currentRunner.time.hSec = TCNT1;		// Get the hundredth of seconds from the timer 1 counter
			}

			addRunner(runnerListHead, currentRunner);	// Add the current runner to the linked list
		}
	}
}

/*
*	time_type averageLapTime(void)
*
*	Returns the average time of all runners laps
*
*	out: time_type - Average time of all laps
*/
static time_type averageLapTime(void)
{
	uint8_t runners = 0;			// Number of runners
	time_type total = {0, 0, 0};	// Initialize total before it is incremented below
	time_type averageTime;
	runner_node* current = runnerListHead;

	while (current != NULL)		// Iterate through the runners
	{
		if (!current -> runner.running)	// If a runner is running the time is it's start time NOT lap time
		{
			runners++;

			total.min += current -> runner.time.min;
			total.sec += current -> runner.time.sec;
			total.hSec += current -> runner.time.hSec;
		}

		current = current -> next;
	}

	if (runners > 1)	// Only execute block if there has actually been a race
	{
		uint8_t tempSec = (total.min * 60) + total.sec; // Temporary turn time into seconds to easier calculate average time
		uint8_t averSec = tempSec / runners;			// Average time in seconds
		averageTime.min = averSec / 60;					// Average time in minutes and
		averageTime.sec = averSec % 60;					// seconds
		averageTime.hSec = total.hSec / runners;		// Hundredths of seconds
	}

	return averageTime;
}

/*
*	runner_node* sortedByTime(runner_node* head)
*
*	Sorts the linked list starting from head by the lowest time
*
*	in:	runner_node* head - Head of linked list to be sorted
*	out: runner_node*  - Head of sorted linked list
*/
static runner_node* sortedByTime(runner_node* head)
{
	runner_node* current = head;
	uint8_t listSize = 0;			// Number of runners in the linked list

	while(current != NULL)			// Count the runners
	{
		listSize++;
		current = current -> next;
	}

	runner_type sortingArray[listSize];		// Temporary array for storing the runner_types that we are going to sort
	runner_node* tempNode = head;

	for (uint8_t i = 0; i < listSize; i++)	// Copy the linked list into the sortingArray
	{
		sortingArray[i] = tempNode -> runner;
		tempNode = tempNode -> next;
	}

	freeRunnerList(head);		// Free the unsorted runner list from heap, it is no longer needed

	for (uint8_t i = listSize - 1; i >= 0; i--)		// Start from the back of the list
	{
		for (uint8_t j = 1; j <= i; j++)			// Loop through each element
		{
			uint8_t time1 = getTimeInSec(sortingArray[j]);		// Get the time of current element and the previous one
			uint8_t time2 = getTimeInSec(sortingArray[j - 1]);

			if (time2 > time1)									// Check if the previous is larger than the current, if it is, they have to be swapped
			{
				runner_type temp = sortingArray[j-1];
				sortingArray[j-1] = sortingArray[j];
				sortingArray[j] = temp;
			}
		 }
	}

	runner_node* sortedHead = malloc(sizeof(runner_node));		// Initialize the head for the finished sorted linked list

	for (uint8_t i = 0; i < listSize; i++)						// Copy the sorted array to a new linked list
	{
		runner_type runner = sortingArray[i];
		addRunner(sortedHead, runner);
	}

	free(sortingArray);

	return sortedHead;											// Return the head of the newly sorted linked list
}


/*
*	void freeRunnerList(runner_node* head)
*
*	Frees the linked list starting at head from the heap
*/
static void freeRunnerList(runner_node* head)
{
	runner_node* previous;
	runner_node* current = head;

	while(current != NULL) {			// Iterate through the linked list
		previous = current;
		current = current -> next;
		free(previous);						// Free the runner_node from the heap
	}
}

/*
*	void outputBest(void)
*
*	Displays the ID to the runner with the best lap time. The ID is displayed on Port A
*/
static void outputBest(void)
{
	if (runnerListHead != NULL)			// Only output when the list of runners is not empty
	{
		uint8_t bestTimeID, bestTimeSec;		// ID of runner with best time and their time in seconds
		runner_node* current = runnerListHead;

		bestTimeID = current -> runner.id;				// Initialize the best to
		bestTimeSec = getTimeInSec(current -> runner);	// be the first in the list

		while(current != NULL)				//Iterate through all runners in list
		{
			if (current -> runner.running)	// Only show runners that have finished running
				break;

			uint8_t timeInSec = getTimeInSec(current -> runner);

			if (bestTimeSec > timeInSec)				// Check if the current runner has a better time than the best
			{
				bestTimeSec = timeInSec;				// if so, the best is now the current runner
				bestTimeID = current -> runner.id;
			}

			current = current -> next;		// Proceed to next node
		}

		PORTA = bestTimeID;		// Output the best runner's ID to Port A
	}
}

/*
*	uint8_t getTimeInSec(runner_node* runnerNode)
*
*	Returns the lap time of a runner in seconds
*
*	in: runner_node* runnerNode - Pointer to the runner
*	out: uint8_t - lap time to runner in seconds
*/
static uint8_t getTimeInSec(runner_type runner)
{
	return (runner.time.min * 60) + runner.time.sec;
}

int main(void)
{
	initTimer();
	initPort();

	runnerListHead = malloc(sizeof(runner_node));	// Initialize the linked list of runners

	while(1)	// Call these functions forever
	{
		inputController();
		outputBest();
	}
	return 0;
}

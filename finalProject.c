
//First define all the pointers for all the memory mapped addresses, create functions that can test their useablity 
//Gonna need double buffering to draw both the static and dynamic elements of the screen 
	//Static is the menu, locks, end screen
	//Dynamic is the lockpins being moved up and down, probably should first start with rectangles 
//Will need to determine the sounds needed to generate (picking, correct/wrong pin) 

#include <stdio.h> 
#include <math.h> 
#include <stdlib.h> 
#include <stdbool.h> 

//Memory mapped addresses  
#define CHAR_BUFFER_BASE 0x09000000 //Character buffer, acts as an overlay on top of the pixel buffer, 80col x 60row grid of character cells
#define KEY_BASE 0xFF200050 
#define PS2_BASE 0xFF200100
#define AUDIO_BASE 0xFF203040
#define SWITCH_BASE 0xFF200040
#define LED_BASE 0xFF200000
#define TIMER_BASE 0xFF202000 
	
volatile int pixel_buffer_start; // global variable
short int buffer1[240][512]; 
short int buffer2[240][512];

//DEFINES/VARIABLES FOR DRAWING 
#define LOCK_BASE_X 60
#define LOCK_BASE_Y 40
#define LOCK_WIDTH 200
#define LOCK_HEIGHT 160
#define CHAMBER_WIDTH 16
#define NUM_PINS 5
int pinYPositions[NUM_PINS]; 
int pickXPosition = 50; //Starting X position of the lockpick 
bool ignoreNext = false; //A flag to catch the 0xF0 key-release byte
bool moveLeft = false;
bool moveRight = false;
	
#define COLOR_WOOD    0x3186  // Dark brown
#define COLOR_BRASS   0xD6A0  // Darker yellow/gold for housing
#define COLOR_GOLD    0xFEA0  // Bright gold for the pins
#define COLOR_BLACK   0x0000  // Empty space
#define COLOR_SPRING  0x7BEF  // Silver/Grey for springs
#define COLOR_PICK    0xCE79  // Dull steel for the lockpick
	
// GLOBAL VARIABLES FOR SWITCH PATTERN
int targetPattern = 0;
int matchedPins = 0;

// GLOBAL VARIABLES FOR INTERRUPT TIMER
volatile int elapsedTime = 0;      // elapsed time in seconds
volatile int timerStarted = 0;     // 0 = not started, 1 = running


//STATES
#define MENU_STATE 0
#define GAME_STATE 1
	
int state = MENU_STATE;

//FUNCTION DECLARATIONS 
//General purpose 
void clearScreen();
void clearCharacter();
void drawRectangle(int x0, int y0, int width, int height, short int color);
void plot_pixel(int x, int y, short int line_color); 
void wait_for_vsync(); 

//Drawing menu 
void writeCharacter(int x, int y, char c); 
void writeString(int x, int y, char *str);
void drawMenu(); 

//Drawing lock 
void drawStaticLock(); 
void drawDynamicElements(); 
int readPS2(char *byte); 


//State changing 
int readKeys(); 
void waitForRelease();

//Audio 
void playStartSound(); 

// Matching switches
int readSwitches();
void updateLEDs(int lightLed);
void matchPins(); // display the correct matched pins (in final game this would not be dipslayed)

// Interrupt timer functions
void set_itimer(void);
void enable_interrupts(void);
void handler(void) __attribute__ ((interrupt ("machine")));
void itimer_ISR(void);


//********************************
// MAIN GAME LOOP
//********************************

int main(void){
	
	volatile int * pixel_ctrl_ptr = (int *)0xFF203020;
    
	//Initialization of the front and back buffers 
	*(pixel_ctrl_ptr + 1) = (int) &buffer1; 
    wait_for_vsync();
	
	pixel_buffer_start = *pixel_ctrl_ptr;
    clearScreen();
	
	*(pixel_ctrl_ptr + 1) = (int) &buffer2;
    pixel_buffer_start = *(pixel_ctrl_ptr + 1); 
    clearScreen();
	clearCharacter();

	set_itimer();
enable_interrupts();

	unsigned int counter = 0;
	
	while(1){
		if(state == MENU_STATE){
			clearScreen();
			drawMenu();
            updateLEDs(0); // Clear LEDs in menu
            timerStarted = 0;
            counter++; 
           
			
			char keyByte; 
			if(readPS2(&keyByte)){
				if(keyByte == 0x5A){
					
					clearCharacter(); 
					
					srand(counter); 
					for(int i = 0; i < NUM_PINS; i++){
                        pinYPositions[i] = 50 + (rand() % 51); 
                    }
					
					targetPattern = rand() & 0x1F;
					matchedPins = 0;
                    
					
					playStartSound(); 

                    elapsedTime = 0;
timerStarted = 1;
					
					state = GAME_STATE;
				} 
			} 
		}
        else if(state == GAME_STATE){
            clearScreen(); 
            
            char keyByte;
            //A while loop reads every byte the keyboard sends this frame for less lag.
            while(readPS2(&keyByte)){
                if(keyByte == (char)0xF0){
                    ignoreNext = true; //Next byte will tell which key was let go 
                } 
				//To ignore the next byte 
                else if(ignoreNext){
					//Stop moving 
                    if(keyByte == (char)0x1C) 
						moveLeft = false;
                    else if(keyByte == (char)0x23) 
						moveRight = false;
                    ignoreNext = false; 
                } 
                else{
					//Key pressed 
					if(keyByte == (char)0x1C) 
						moveLeft = true;
                    else if(keyByte == (char)0x23)
						moveRight = true;
                }
            }
            
            //Move the pick  
            int pickSpeed = 8; // Increase this number to make the pick fly!
            
            if(moveLeft){
                pickXPosition -= pickSpeed; 
                if(pickXPosition < 20) 
					pickXPosition = 20; 
            }
            if(moveRight){
                pickXPosition += pickSpeed; 
                if(pickXPosition > 250) 
					pickXPosition = 250; 
            }
            
			matchPins();
            drawStaticLock(); 
            drawDynamicElements();
            updateLEDs(elapsedTime);
        }
		

		wait_for_vsync(); 
        pixel_buffer_start = *(pixel_ctrl_ptr + 1);
	}
}

void drawMenu(){
	drawRectangle(80, 40, 160, 160, 0x001F); //Blue background rectangle 
    
    //Highlight box for STARTGAME 
    drawRectangle(100, 110, 120, 24, 0x07E0); // 0x07E0 is Green
    
    //Write the text 
    writeString(30, 15, "WELCOME TO LOCK PICK");   
    writeString(35, 29, "START GAME");    
;
}

//Clears the entire screen black at the start 
void clearScreen(){
	int x = 320; 
	int y = 240; 
	for(int i = 0; i < x; i ++){
		for(int j = 0; j < y; j++){
			plot_pixel(i, j, 0x0000); 
		}
	}
}

//Writes character 
void writeCharacter(int x, int y, char c) {
    // Character buffer addressing shifts y by 7 (multiplying by 128)
    volatile char *character_buffer = (char *) (CHAR_BUFFER_BASE + (y << 7) + x);
    *character_buffer = c;
}

//Iterates through str and writes each character out horizontally 
void writeString(int x, int y, char *str) {
    int i = 0;
    while (str[i] != '\0') {
        writeCharacter(x + i, y, str[i]);
        i++;
    }
}

//Draws rectangle with height & width at starting position (x0, y0) 
void drawRectangle(int x0, int y0, int width, int height, short int color) {
    for (int x = x0; x < x0 + width; x++) {
        for (int y = y0; y < y0 + height; y++) {
            plot_pixel(x, y, color);
        }
    }
}

//Plots pixel 
void plot_pixel(int x, int y, short int line_color){    
	volatile short int *one_pixel_address;
    one_pixel_address = pixel_buffer_start + (y << 10) + (x << 1); 
    *one_pixel_address = line_color;
}

//Since character buffer overlays the pixel buffer, must clear characters alongside pixels 
//Draws ' ' on all character cells in the character grid 
void clearCharacter() {
    for (int x = 0; x < 80; x++) {
        for (int y = 0; y < 60; y++) {
            writeCharacter(x, y, ' '); // Write a blank space
        }
    }
}

//Draws the static lock 
void drawStaticLock() {
    //Background for the entire screen 
    drawRectangle(0, 0, 320, 240, COLOR_WOOD);

    //Back plate 
    drawRectangle(LOCK_BASE_X + 20, LOCK_BASE_Y - 20, LOCK_WIDTH - 40, 20, COLOR_BRASS); 
    //Heavy main body 
    drawRectangle(LOCK_BASE_X, LOCK_BASE_Y, LOCK_WIDTH, LOCK_HEIGHT, COLOR_BRASS); 

    //Horizontal pick 
    drawRectangle(20, 130, 280, 24, COLOR_BLACK);

    //5 pin chambers and their springs 
    for(int i = 0; i < NUM_PINS; i++) {
        //Space them out evenly across the lock base
        int chamber_x = LOCK_BASE_X + 25 + (i * 32);
        
        //Draw the empty black chamber track extending upwards
        drawRectangle(chamber_x, LOCK_BASE_Y + 10, CHAMBER_WIDTH, 80, COLOR_BLACK);
        
        
    }
}

// Assuming these variables are declared globally in the main game loop:
// int pinYPosition[NUM_PINS]; // Ranging roughly from Y=90 (up) to Y=110 (resting down)
// int pick_x_position;           // Ranging from X=30 to X=200

void drawDynamicElements() {
    //Draw the dynamic pins at their current Y heights
    for(int i = 0; i < NUM_PINS; i++) {
        int pinX = LOCK_BASE_X + 25 + (i * 32);
        int currentY = pinYPositions[i]; //Should be current_y = pin_y_position[NUM_PINS] ranging from 90/110 
		int blockTopY = LOCK_BASE_Y + 10; 
        
		//Draw the static spring at the top of the chamber
        //Use a quick loop to draw alternating lines to simulate coil gaps
        for(int spring = blockTopY; spring < currentY; spring += 6) {
            drawRectangle(pinX + 2, spring, CHAMBER_WIDTH - 4, 3, COLOR_SPRING);
        }
		
        //Draw the main body of the pin
        drawRectangle(pinX + 2, currentY, CHAMBER_WIDTH - 4, 26, COLOR_GOLD); 
        
        //Draw a smaller rectangle at the bottom for rounded pin tip
        drawRectangle(pinX + 4, currentY + 26, CHAMBER_WIDTH - 8, 4, COLOR_GOLD);
		
    }
    
    //Draw the lockpick coming from the left edge of the screen
    //The long thin shaft extending to the current X position
    drawRectangle(0, 142, pickXPosition, 4, COLOR_PICK); 
    
    //Draw the pick tip (the hook curving up to interact with pins)
    drawRectangle(pickXPosition, 136, 4, 10, COLOR_PICK);
    drawRectangle(pickXPosition - 4, 136, 4, 4, COLOR_PICK);
}

int readKeys() {
    volatile int *keyPtr = (int *)KEY_BASE;
    return *keyPtr; 
}

//Pauses the game until the user takes their finger off the button
void waitForRelease() {
    while (readKeys() != 0) {
        // Do nothing, just wait
    }
}

//Reads a single byte from the PS/2 keyboard buffer
//Returns 1 if a key was pressed, 0 if the buffer is empty
int readPS2(char *byte) {
    volatile int *ps2Ptr = (int *)PS2_BASE;
    int ps2Data = *ps2Ptr; //Gets the input from the keyboard 
    
    //Bit 15 is the RVALID (Read Valid) flag.
    //If it is 1, there is valid keyboard data in the lowest 8 bits.
    if (ps2Data & 0x8000) {
        *byte = ps2Data & 0xFF; //Extract the hex Make Code
        return 1;
    }
    return 0;
}

void wait_for_vsync(){
	volatile int *pixel_ctrl_ptr = (int *)0xFF203020; 
    register int status;
	
	*pixel_ctrl_ptr = 1; //Store a 1 into the Buffer 

	//wait for status = 0 
    status = *(pixel_ctrl_ptr + 3); 
    while ((status & 0x01) != 0) { 
        status = *(pixel_ctrl_ptr + 3);
    }
}

void playStartSound() {
    volatile int *audioPtr = (int *)AUDIO_BASE;
    int leftFifoSpace;
    int rightFifoSpace;
    
    // Audio parameters
    int sampleRate = 48000;
    int freq = 1760; // High pitch frequency in Hz
    int halfPeriod = sampleRate / freq / 2;
    int volume = 0x00FFFFFF; // High volume, but avoids clipping the speakers
    
    int durationSamples = sampleRate / 4; // Play for exactly 0.25 seconds
    int currentSample = 0;
    int waveCounter = 0;
    int currentAmplitude = volume;
    
    // This loop blocks the game for 0.25s while it plays the sound
    while (currentSample < durationSamples) {
        // Read the FIFOSpace register (offset +1)
        int fifoSpace = *(audioPtr + 1);
        
        // Extract the available space for left (bits 24-31) and right (bits 16-23)
        leftFifoSpace = (fifoSpace >> 24) & 0xFF;
        rightFifoSpace = (fifoSpace >> 16) & 0xFF;
        
        // Only write if there is physical room in both hardware buffers
        if (leftFifoSpace > 0 && rightFifoSpace > 0) {
            
            // Write the current wave amplitude to Left (offset +2) and Right (offset +3)
            *(audioPtr + 2) = currentAmplitude;
            *(audioPtr + 3) = currentAmplitude;
            
            currentSample++;
            waveCounter++;
            
            // Toggle the square wave up and down based on the frequency
            if (waveCounter >= halfPeriod) {
                currentAmplitude = -currentAmplitude; // Flip the wave
                waveCounter = 0;
            }
        }
    }
}


// Read the switches 0-3
int readSwitches(){
	volatile int* sw = (int *)SWITCH_BASE;
	return (*sw) & 0x1F;
}

// Show LEDs

void updateLEDs(int ledToLight){
	volatile int* led = (int *)LED_BASE;
	*led = ledToLight;
}

// Mtch the pins and display on led

void matchPins(){
	int currentSwitches = readSwitches();
	matchedPins = 0;
	
	for (int pin = 0; pin < NUM_PINS; pin++){
		int switchToggled = (currentSwitches >> pin) & 1;
		int patternBit = (targetPattern >> pin) & 1;
		if (switchToggled == patternBit){
			matchedPins |= (1 << pin);
        }
	}
	
//	updateLEDs(matchedPins);
}


// Timer functions
void set_itimer(void) {
    volatile int *timer_ptr = (int *)TIMER_BASE;

    int counterValue = 100000000;  // 1 second at 100 MHz

    // Clear any pending timer interrupt first
    *timer_ptr = 0;

    // Load the 32-bit starting count into the timer period registers
    *(timer_ptr + 2) = counterValue & 0xFFFF;         // low 16 bits
    *(timer_ptr + 3) = (counterValue >> 16) & 0xFFFF; // high 16 bits

    // Start timer, continuous mode, interrupt enabled
    *(timer_ptr + 1) = 0x7;   // START + CONT + ITO
}

void enable_interrupts(void) {
    int mstatus_value, mtvec_value, mie_value;

    mstatus_value = 0b1000;   // machine interrupt enable bit in mstatus

    // Disable global interrupts first while setting everything up
    __asm__ volatile ("csrc mstatus, %0" :: "r"(mstatus_value));

    // Set trap handler address
    mtvec_value = (int)&handler;
    __asm__ volatile ("csrw mtvec, %0" :: "r"(mtvec_value));

    // Disable all currently enabled interrupts
    __asm__ volatile ("csrr %0, mie" : "=r"(mie_value));
    __asm__ volatile ("csrc mie, %0" :: "r"(mie_value));

    // Enable only the interval timer interrupt (IRQ 16 -> bit 16)
    mie_value = 0x10000;
    __asm__ volatile ("csrs mie, %0" :: "r"(mie_value));

    // Enable global machine interrupts
    __asm__ volatile ("csrs mstatus, %0" :: "r"(mstatus_value));
}

void handler(void) {
    int mcause_value;

    __asm__ volatile ("csrr %0, mcause" : "=r"(mcause_value));

    if (mcause_value == 0x80000010) {   // interval timer interrupt
        itimer_ISR();
    }
    // else ignore other traps for now
}

void itimer_ISR(void) {
    volatile int *timer_ptr = (int *)TIMER_BASE;

    // Clear the timer interrupt
    *timer_ptr = 0;

    // Only count time while the game is actually running
    if (state == GAME_STATE && timerStarted == 1) {
        elapsedTime++;
    }
}
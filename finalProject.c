// First define all the pointers for all the memory mapped addresses, create
// functions that can test their useablity Gonna need double buffering to draw
// both the static and dynamic elements of the screen Static is the menu, locks,
// end screen Dynamic is the lockpins being moved up and down, probably should
// first start with rectangles
// Will need to determine the sounds needed to generate (picking, correct/wrong
// pin)

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

// Memory mapped addresses
#define CHAR_BUFFER_BASE \
  0x09000000 // Character buffer, acts as an overlay on top of the pixel
             // buffer, 80col x 60row grid of character cells
#define KEY_BASE 0xFF200050
#define PS2_BASE 0xFF200100
#define AUDIO_BASE 0xFF203040
#define SWITCH_BASE 0xFF200040
#define LED_BASE 0xFF200000
#define TIMER_BASE 0xFF202000

volatile int pixel_buffer_start; // global variable
short int buffer1[240][512];
short int buffer2[240][512];

// ROTARY
#define ROTARY_BASE 0xFF200060
#define JP1_MCAUSE 0x8000001B // Machine External Interrupt (for rotary)

#define LEDR_ptr ((volatile int *)LED_BASE)
#define rotary_ptr ((volatile int *)ROTARY_BASE)

#define JP1_DATA (*(rotary_ptr))
#define JP1_DIR (*(rotary_ptr + 1))
#define JP1_IE (*(rotary_ptr + 2))
#define JP1_EDGE (*(rotary_ptr + 3))

volatile int rotary_counter = 0;

void rotary_ISR(void);

// ROTARY FOR GAME PLAY
int target_rotary_start = 30;
int target_rotary_end = 40;
bool rotary_in_range = false;

// DEFINES/VARIABLES FOR DRAWING
#define LOCK_BASE_X 60
#define LOCK_BASE_Y 40
#define LOCK_WIDTH 200
#define LOCK_HEIGHT 160
#define CHAMBER_WIDTH 16
#define NUM_PINS 5
int pinYPositions[NUM_PINS];
int pickXPosition = 50;  // Starting X position of the lockpick
bool ignoreNext = false; // A flag to catch the 0xF0 key-release byte
bool moveLeft = false;
bool moveRight = false;

#define COLOR_WOOD 0x3186   // Dark brown
#define COLOR_BRASS 0xD6A0  // Darker yellow/gold for housing
#define COLOR_GOLD 0xFEA0   // Bright gold for the pins
#define COLOR_BLACK 0x0000  // Empty space
#define COLOR_SPRING 0x7BEF // Silver/Grey for springs
#define COLOR_PICK 0xCE79   // Dull steel for the lockpick

// GLOBAL VARIABLES FOR SWITCH PATTERN
int targetPattern = 0;
int matchedPins = 0;

// GLOBAL VARIABLES FOR TIMER
volatile int elapsedTime = 0;
volatile int timerStarted = 0; // 0 not started

// STATES
#define MENU_STATE 0
#define GAME_STATE 1
#define END_STATE 2

int state = MENU_STATE;

// DIFFICULTIES
#define DIFF_EASY 0
#define DIFF_MEDIUM 1
#define DIFF_HARD 2

int menuSelection = DIFF_EASY;
int gameDifficulty = DIFF_EASY;
bool gameWon = false;

// FUNCTION DECLARATIONS
// General purpose
void clearScreen();
void clearCharacter();
void drawRectangle(int x0, int y0, int width, int height, short int color);
void plot_pixel(int x, int y, short int line_color);
void wait_for_vsync();

// Drawing menu
void writeCharacter(int x, int y, char c);
void writeString(int x, int y, char *str);
void drawMenu();
void drawEndScreen();

// Drawing lock
void drawStaticLock();
void drawDynamicElements();
int readPS2(char *byte);
void eraseDynamicElements();

// State changing
int readKeys();
void waitForRelease();

// Audio
void playStartSound();
void playSuccessSound();
void playFailSound();

// Matching switches
int readSwitches();
void updateLEDs(int lightLed);
void matchPins(); // display the correct matched pins (in final game this would
                  // not be dipslayed)

// Timer functions
static void handler(void) __attribute__((interrupt("machine")));
void set_itimer(void);        // initialize timer
void enable_interrupts(void); // enable interrupts
void itimer_ISR(void);
void drawTimer();

#define PIN_REST_Y 110
// Delete: #define SHEAR_LINE_Y 75
int pinTargetY[NUM_PINS];  // Stores the unique red line height for each pin
#define PIN_REST_Y 110

// Game Design
int currentPinIndex = 0;
bool isHoldingW = false; // Tracks if pin is being held
bool pinSet[NUM_PINS] = {false, false, false, false,
                         false}; // Tracks which pins are picked
int pinSequence[NUM_PINS];
int currentSequenceIndex =
    0; // Tracks which step of the sequence the player is on

// ROTARY FUNCTIONS
void drawRotaryBar();

// --- AUDIO ENGINE GLOBALS ---
int audioSamplesRemaining = 0;
int audioCurrentFreq = 0;
int audioWaveCounter = 0;
int audioCurrentAmplitude = 0;
int audioPhase = 0;  // Tracks the 3 parts of the "Ding Ding" success sound

void triggerSuccessSound() {
  // Start Phase 1: First Ding (2093 Hz for 0.1s)
  audioSamplesRemaining = 8000 / 10;
  audioCurrentFreq = 2093;
  audioWaveCounter = 0;
  audioCurrentAmplitude = 0x00FFFFFF;
  audioPhase = 1;
}

void triggerFailSound() {
  // Single Phase: Low Buzz (150 Hz for 0.33s)
  audioSamplesRemaining = 8000 / 3;
  audioCurrentFreq = 150;
  audioWaveCounter = 0;
  audioCurrentAmplitude = 0x00FFFFFF;
  audioPhase = 0;
}

void updateAudio() {
  // If a sound just finished, check if there's a next phase (for the Success
  // Ding)
  if (audioSamplesRemaining <= 0) {
    if (audioPhase == 1) {
      // Phase 2: Silence for 0.05s
      audioSamplesRemaining = 8000 / 20;
      audioCurrentFreq = 0;
      audioPhase = 2;
    } else if (audioPhase == 2) {
      // Phase 3: Second Higher Ding (2637 Hz for 0.15s)
      audioSamplesRemaining = (8000 * 15) / 100;
      audioCurrentFreq = 2637;
      audioCurrentAmplitude = 0x00FFFFFF;
      audioWaveCounter = 0;
      audioPhase = 3;
    } else {
      // Sound is completely finished
      return;
    }
  }

  volatile int* audioPtr = (int*)AUDIO_BASE;
  int fifoSpace = *(audioPtr + 1);
  int leftSpace = (fifoSpace >> 24) & 0xFF;
  int rightSpace = (fifoSpace >> 16) & 0xFF;

  // Find out exactly how much room the hardware has right now
  int spaceToWrite = (leftSpace < rightSpace) ? leftSpace : rightSpace;

  // Don't write more samples than the sound actually needs
  if (spaceToWrite > audioSamplesRemaining) {
    spaceToWrite = audioSamplesRemaining;
  }

  // Rapidly fill the available space and get out!
  for (int i = 0; i < spaceToWrite; i++) {
    if (audioCurrentFreq == 0) {
      *(audioPtr + 2) = 0;  // Write Silence
      *(audioPtr + 3) = 0;
    } else {
      *(audioPtr + 2) = audioCurrentAmplitude;  // Write Tone
      *(audioPtr + 3) = audioCurrentAmplitude;

      audioWaveCounter++;
      int halfPeriod = 8000 / audioCurrentFreq / 2;
      if (audioWaveCounter >= halfPeriod) {
        audioCurrentAmplitude = -audioCurrentAmplitude;
        audioWaveCounter = 0;
      }
    }
    audioSamplesRemaining--;
  }
}

int main(void) {
  volatile int* pixel_ctrl_ptr = (int*)0xFF203020;

  // Initialization of the front and back buffers
  *(pixel_ctrl_ptr + 1) = (int)&buffer1;
  wait_for_vsync();

  pixel_buffer_start = *pixel_ctrl_ptr;
  clearScreen();

  *(pixel_ctrl_ptr + 1) = (int)&buffer2;
  pixel_buffer_start = *(pixel_ctrl_ptr + 1);
  clearScreen();
  clearCharacter();

  set_itimer();
  enable_interrupts();

  unsigned int counter = 0;

  updateLEDs(0); // Clear LEDs in menu

  while (1)
  {
    if (state == MENU_STATE)
    {
      clearScreen();
      drawMenu();
      timerStarted = 0;
      counter++;

      char keyByte;
      while (readPS2(&keyByte))
      {
        if (keyByte == (char)0xF0)
          ignoreNext = true; // Key release coming up
        else if (ignoreNext)
          ignoreNext = false; // Ignore key releases in the menu
        else
        {
          // KEY PRESSED
          if (keyByte == (char)0x1D)
          {
            //'W' pressed
            if (menuSelection > 0)
              menuSelection--;
          }
          else if (keyByte == (char)0x1B)
          {
            //'S' pressed
            if (menuSelection < 2)
              menuSelection++;
          }
          else if (keyByte == (char)0x5A)
          {
            // Enter pressed, start the game with the selected difficulty
            gameDifficulty = menuSelection;

            clearCharacter();

            srand(counter);

                        // Generate 5 perfectly unique random pins out of thin air
            for (int i = 0; i < NUM_PINS; i++) {
              int randomPin;
              bool isDuplicate;

              do {
                randomPin = rand() % NUM_PINS;
                isDuplicate = false;
                for (int j = 0; j < i; j++) {
                  if (pinSequence[j] == randomPin) {
                    isDuplicate = true;
                    break;
                  }
                }
              } while (isDuplicate);

              pinSequence[i] = randomPin;
              pinYPositions[i] = PIN_REST_Y;
              pinSet[i] = false;

              // Generates a random target height between 60 and 90
              pinTargetY[i] = 70 + (rand() % 31);
            }

            currentSequenceIndex = 0;
            // targetPattern = rand() & 0x1F;
            matchedPins = 0;

            playStartSound();

            elapsedTime = 0;
            timerStarted = 1;

            /*RANDOMIZE TARGET FOR ROTARY*/

            // Randomize starting position, but keep it at least 30 away from the zone
            do
            {
              rotary_counter = rand() % 256;
            } while (rotary_counter >= target_rotary_start - 30 &&
                     rotary_counter <= target_rotary_end + 30);

            /*END OF RANDOMIZE TARGET FOR ROTARY*/

            // Draw the heavy lock to both buffers once
            pixel_buffer_start = (int)&buffer1;
            drawStaticLock();

            pixel_buffer_start = (int)&buffer2;
            drawStaticLock();

            state = GAME_STATE;
          }
        }
      }
    }
    else if (state == GAME_STATE)
    {
      char keyByte;
      while (readPS2(&keyByte))
      {
        if (keyByte == (char)0xF0)
        {
          ignoreNext = true;
        }
        else if (ignoreNext)
        {
          ignoreNext = false;

          // Key released is W
          if (keyByte == (char)0x1D)
          {
            isHoldingW = false;
            // Was W release it while the gap was on the line?
            int margin = 3;
            if (gameDifficulty == DIFF_MEDIUM)
              margin = 2;
            else if (gameDifficulty == DIFF_HARD)
              margin = 1;
            if (!pinSet[currentPinIndex]) {
              // Check against THIS specific pin's random target height!
              if (pinYPositions[currentPinIndex] >=
                      pinTargetY[currentPinIndex] - margin &&
                  pinYPositions[currentPinIndex] <=
                      pinTargetY[currentPinIndex] + margin) {
                // --- THE SEQUENCE CHECK ---
                if (currentPinIndex == pinSequence[currentSequenceIndex]) {
                  pinSet[currentPinIndex] = true;
                  currentSequenceIndex++;
                  triggerSuccessSound();
                } else {
                  for (int i = 0; i < NUM_PINS; i++) {
                    pinSet[i] = false;
                  }
                  currentSequenceIndex = 0;
                  triggerFailSound();
                }
              }
            }
          }
        }
        else
        {
          // Key pressed
          if (keyByte == (char)0x1C && !isHoldingW)
          {
            if (currentPinIndex > 0)
              currentPinIndex--; // Move lockpick Left
          }
          else if (keyByte == (char)0x23 && !isHoldingW)
          {
            if (currentPinIndex < NUM_PINS - 1)
              currentPinIndex++; // Move lockpick Right
          }
          else if (keyByte == (char)0x1D)
          {
            // Press 'W' to start lifting, but only if it's not already set
            if (!pinSet[currentPinIndex] && rotary_in_range)
            {
              isHoldingW = true;
            }
          }
        }
      }

      // Game physics
      if (isHoldingW)
      {
        // Lift the pin while holding W
        if (rotary_in_range)
        {
          pinYPositions[currentPinIndex] -= 1;
        if (pinYPositions[currentPinIndex] < 65)
          pinYPositions[currentPinIndex] = 65;  // Ceiling
        }
        else
        {
          isHoldingW = false; // If they move out of the rotary range while lifting, drop the pin immediately
        }
      }
      else
      {
        // Gravity
        for (int i = 0; i < NUM_PINS; i++)
        {
          if (!pinSet[i] && pinYPositions[i] < PIN_REST_Y)
          {
            pinYPositions[i] += 4; // Fall fast
            if (pinYPositions[i] > PIN_REST_Y)
              pinYPositions[i] = PIN_REST_Y;
          }
        }
      }

      eraseDynamicElements();

      // Snap pick position
      pickXPosition = LOCK_BASE_X + 32 + (currentPinIndex * 32);

      drawDynamicElements();

      int maxTime = 180;
      if (gameDifficulty == DIFF_MEDIUM)
        maxTime = 120;
      if (gameDifficulty == DIFF_HARD)
        maxTime = 60;

      int setTotal = 0;
      for (int i = 0; i < NUM_PINS; i++)
      {
        if (pinSet[i])
          setTotal++;
      }

      if (setTotal == NUM_PINS)
      {
        timerStarted = 0;
        gameWon = true;
        clearCharacter();
        state = END_STATE;
      }
      else if (elapsedTime >= maxTime)
      {
        timerStarted = 0;
        gameWon = false;
        clearCharacter();
        state = END_STATE;
      }
      else
      {
        // Game is still actively running
        // updateLEDs(elapsedTime);
        drawTimer();
        drawRotaryBar();
      }
    }
    else if (state == END_STATE)
    {
      clearScreen();
      drawEndScreen();

      char keyByte;
      // Drain the PS/2 buffer looking for the Enter key
      while (readPS2(&keyByte))
      {
        if (keyByte == (char)0xF0)
        {
          ignoreNext = true;
        }
        else if (ignoreNext)
        {
          ignoreNext = false;
        }
        else if (keyByte == 0x5A)
        {
          // Enter Key Pressed Loop back to the start!
          clearCharacter();
          state = MENU_STATE;
        }
      }
    }

    updateAudio();
    wait_for_vsync();
    pixel_buffer_start = *(pixel_ctrl_ptr + 1);
  }
}

void drawMenu()
{
  // Solid blue background rectangle
  drawRectangle(80, 40, 160, 160, 0x001F);

  // Main Title
  writeString(30, 15, "WELCOME TO LOCK PICK");

  // Calculate the positions for the 3 menu options
  int baseY = 90;
  int boxHeight = 24;
  int spacing = 35;

  // Draw the highlight boxes
  for (int i = 0; i < 3; i++)
  {
    if (i == menuSelection)
    {
      // Draw GREEN highlight if it is the currently selected option
      drawRectangle(100, baseY + (i * spacing), 120, boxHeight, 0x07E0);
    }
    else
    {
      // Draw BLUE background to clear out the old highlight
      drawRectangle(100, baseY + (i * spacing), 120, boxHeight, 0x001F);
    }
  }

  // Write the text directly over the boxes
  writeString(38, 24, "EASY");
  writeString(37, 33, "MEDIUM");
  writeString(38, 41, "HARD");
}

void drawEndScreen()
{
  // Draw a solid black background
  drawRectangle(0, 0, 320, 240, COLOR_BLACK);

  if (gameWon)
  {
    // Game won
    drawRectangle(60, 80, 200, 80, COLOR_GOLD);
    writeString(34, 23, "LOCK PICKED!");

    char timeMsg[30];
    snprintf(timeMsg, sizeof(timeMsg), "FINAL TIME: %03d SEC", elapsedTime);
    writeString(30, 26, timeMsg);
  }
  else
  {
    // Game lost (0xF800 is pure red in 16-bit color)
    drawRectangle(60, 80, 200, 80, 0xF800);
    writeString(35, 23, "TIME'S UP!");
    writeString(31, 26, "YOU FAILED TO PICK THE LOCK");
  }

  // Prompt to play again
  writeString(29, 32, "PRESS ENTER TO RESTART");
}

// Clears the entire screen black at the start
void clearScreen()
{
  int x = 320;
  int y = 240;
  for (int i = 0; i < x; i++)
  {
    for (int j = 0; j < y; j++)
    {
      plot_pixel(i, j, 0x0000);
    }
  }
}

// Formats the elapsed time and prints it to the top right of the character
// buffer
void drawTimer()
{
  char timeStr[20];

  // Format the string. "%03d" ensures it always takes up 3 digits (e.g., 001,
  // 015, 120)
  snprintf(timeStr, sizeof(timeStr), "TIME: %03d", elapsedTime);

  // X = 68 (near the right edge of the 80-column grid)
  // Y = 2  (near the top of the 60-row grid)
  writeString(68, 2, timeStr);
}

// Writes character
void writeCharacter(int x, int y, char c)
{
  // Character buffer addressing shifts y by 7 (multiplying by 128)
  volatile char *character_buffer = (char *)(CHAR_BUFFER_BASE + (y << 7) + x);
  *character_buffer = c;
}

// Iterates through str and writes each character out horizontally
void writeString(int x, int y, char *str)
{
  int i = 0;
  while (str[i] != '\0')
  {
    writeCharacter(x + i, y, str[i]);
    i++;
  }
}

// Draws rectangle with height & width at starting position (x0, y0)
void drawRectangle(int x0, int y0, int width, int height, short int color)
{
  for (int x = x0; x < x0 + width; x++)
  {
    for (int y = y0; y < y0 + height; y++)
    {
      plot_pixel(x, y, color);
    }
  }
}

// Plots pixel
void plot_pixel(int x, int y, short int line_color)
{
  volatile short int *one_pixel_address;
  one_pixel_address = pixel_buffer_start + (y << 10) + (x << 1);
  *one_pixel_address = line_color;
}

// Since character buffer overlays the pixel buffer, must clear characters
// alongside pixels Draws ' ' on all character cells in the character grid
void clearCharacter()
{
  for (int x = 0; x < 80; x++)
  {
    for (int y = 0; y < 60; y++)
    {
      writeCharacter(x, y, ' '); // Write a blank space
    }
  }
}

// Draws the static lock
void drawStaticLock() {
  // Declare the variables locally inside the function!
  int lineThickness = 3;
  int marginOffset = 3;  // <-- Capital 'O'

  if (gameDifficulty == DIFF_MEDIUM) {
    lineThickness = 2;
    marginOffset = 2;
  } else if (gameDifficulty == DIFF_HARD) {
    lineThickness = 1;
    marginOffset = 1;
  }

  // Background for the entire screen
  drawRectangle(0, 0, 320, 240, COLOR_WOOD);

  // Back plate
  drawRectangle(LOCK_BASE_X + 20, LOCK_BASE_Y - 20, LOCK_WIDTH - 40, 20,
                COLOR_BRASS);
  // Heavy main body
  drawRectangle(LOCK_BASE_X, LOCK_BASE_Y, LOCK_WIDTH, LOCK_HEIGHT, COLOR_BRASS);

  // Horizontal pick
  drawRectangle(20, 130, 280, 24, COLOR_BLACK);

  // 5 pin chambers and their springs
  for (int i = 0; i < NUM_PINS; i++) {
    // Space them out evenly across the lock base
    int chamber_x = LOCK_BASE_X + 25 + (i * 32);

    // Draw the empty black chamber track extending upwards
    drawRectangle(chamber_x, LOCK_BASE_Y, CHAMBER_WIDTH, 90, COLOR_BLACK);
    drawRectangle(chamber_x, pinTargetY[i] - marginOffset, CHAMBER_WIDTH,
                  lineThickness, 0xF800);
  }
}

// Assuming these variables are declared globally in the main game loop:
// int pinYPosition[NUM_PINS]; // Ranging roughly from Y=90 (up) to Y=110
// (resting down) int pick_x_position;           // Ranging from X=30 to X=200

void drawDynamicElements() {
  int lineThickness = 3;
  int marginOffset = 3;

  if (gameDifficulty == DIFF_MEDIUM)
  {
    lineThickness = 2;
    marginOffset = 2;
  }
  else if (gameDifficulty == DIFF_HARD)
  {
    lineThickness = 1;
    marginOffset = 1;
  }

  for (int i = 0; i < NUM_PINS; i++) {
    int pinX = LOCK_BASE_X + 25 + (i * 32);
    int currentY = pinYPositions[i];
    int targetY = pinTargetY[i];  // Grab this pin's specific target!

    // 1. Draw the unique red line for this specific chamber
    drawRectangle(pinX, targetY - marginOffset, CHAMBER_WIDTH, lineThickness,
                  0xF800);

    // 2. Draw the Pins
    if (pinSet[i]) {
      int pinSectionHeight = 20;

      // Top driver pin sits precisely on the TOP edge of this pin's red line
      int topPinBottomEdge = targetY - marginOffset;
      drawRectangle(pinX + 2, topPinBottomEdge - pinSectionHeight,
                    CHAMBER_WIDTH - 4, pinSectionHeight, COLOR_GOLD);

      // Bottom key pin sits precisely on the BOTTOM edge
      int bottomPinTopEdge = topPinBottomEdge + lineThickness;
      drawRectangle(pinX + 2, bottomPinTopEdge, CHAMBER_WIDTH - 4,
                    pinSectionHeight, COLOR_GOLD);

    } else {
      // PIN IS UNPICKED
      drawRectangle(pinX + 2, currentY - 22, CHAMBER_WIDTH - 4, 20, COLOR_GOLD);
      drawRectangle(pinX + 2, currentY, CHAMBER_WIDTH - 4, 20, COLOR_GOLD);
    }
  }

  // Draw the lockpick coming from the left edge of the screen
  drawRectangle(0, 142, pickXPosition, 4, COLOR_PICK);

  int tipTopY = 136;
  if (isHoldingW) {
    tipTopY = pinYPositions[currentPinIndex] + 20;
  }

  drawRectangle(pickXPosition, tipTopY, 4, 142 - tipTopY, COLOR_PICK);
  drawRectangle(pickXPosition - 4, tipTopY, 4, 4, COLOR_PICK);
}

int readKeys()
{
  volatile int *keyPtr = (int *)KEY_BASE;
  return *keyPtr;
}

// Pauses the game until the user takes their finger off the button
void waitForRelease()
{
  while (readKeys() != 0)
  {
    // Do nothing, just wait
  }
}

// Paints over the specific tracks to erase the old pick and springs
// without having to redraw the heavy wood and brass background.
void eraseDynamicElements() {
  // Declare the variables locally inside the function!
  int lineThickness = 3;
  int marginOffset = 3;  // <-- Capital 'O'

  if (gameDifficulty == DIFF_MEDIUM) {
    lineThickness = 2;
    marginOffset = 2;
  } else if (gameDifficulty == DIFF_HARD) {
    lineThickness = 1;
    marginOffset = 1;
  }
  // 1. Patch the wooden background on the far left (where the pick slides in)
  drawRectangle(0, 130, 20, 24, COLOR_WOOD);

  // 2. Patch the horizontal lockpick keyhole track
  drawRectangle(20, 130, 280, 24, COLOR_BLACK);

  // 3. Patch the vertical pin chambers
  for (int i = 0; i < NUM_PINS; i++)
  {
    int chamber_x = LOCK_BASE_X + 25 + (i * 32);
    drawRectangle(chamber_x, LOCK_BASE_Y, CHAMBER_WIDTH, 90, COLOR_BLACK);
    drawRectangle(chamber_x, pinTargetY[i] - marginOffset, CHAMBER_WIDTH,
                  lineThickness, 0xF800);
  }
}

// Reads a single byte from the PS/2 keyboard buffer
// Returns 1 if a key was pressed, 0 if the buffer is empty
int readPS2(char *byte)
{
  volatile int *ps2Ptr = (int *)PS2_BASE;
  int ps2Data = *ps2Ptr; // Gets the input from the keyboard

  // Bit 15 is the RVALID (Read Valid) flag.
  // If it is 1, there is valid keyboard data in the lowest 8 bits.
  if (ps2Data & 0x8000)
  {
    *byte = ps2Data & 0xFF; // Extract the hex Make Code
    return 1;
  }
  return 0;
}

void wait_for_vsync()
{
  volatile int *pixel_ctrl_ptr = (int *)0xFF203020;
  register int status;

  *pixel_ctrl_ptr = 1; // Store a 1 into the Buffer

  // wait for status = 0
  status = *(pixel_ctrl_ptr + 3);
  while ((status & 0x01) != 0)
  {
    status = *(pixel_ctrl_ptr + 3);
  }
}

void playStartSound()
{
  volatile int *audioPtr = (int *)AUDIO_BASE;
  int leftFifoSpace;
  int rightFifoSpace;

  // Audio parameters
  int sampleRate = 8000;
  int freq = 1760; // High pitch frequency in Hz
  int halfPeriod = sampleRate / freq / 2;
  int volume = 0x00FFFFFF; // High volume, but avoids clipping the speakers

  int durationSamples = sampleRate / 4; // Play for exactly 0.25 seconds
  int currentSample = 0;
  int waveCounter = 0;
  int currentAmplitude = volume;

  // This loop blocks the game for 0.25s while it plays the sound
  while (currentSample < durationSamples)
  {
    // Read the FIFOSpace register (offset +1)
    int fifoSpace = *(audioPtr + 1);

    // Extract the available space for left (bits 24-31) and right (bits 16-23)
    leftFifoSpace = (fifoSpace >> 24) & 0xFF;
    rightFifoSpace = (fifoSpace >> 16) & 0xFF;

    // Only write if there is physical room in both hardware buffers
    if (leftFifoSpace > 0 && rightFifoSpace > 0)
    {
      // Write the current wave amplitude to Left (offset +2) and Right (offset
      // +3)
      *(audioPtr + 2) = currentAmplitude;
      *(audioPtr + 3) = currentAmplitude;

      currentSample++;
      waveCounter++;

      // Toggle the square wave up and down based on the frequency
      if (waveCounter >= halfPeriod)
      {
        currentAmplitude = -currentAmplitude; // Flip the wave
        waveCounter = 0;
      }
    }
  }
}

void playSuccessSound()
{
  volatile int *audioPtr = (int *)AUDIO_BASE;

  // Audio parameters configured for CPUlator
  int sampleRate = 8000;
  int volume = 0x00FFFFFF;

  int freq1 = 2093;
  int halfPeriod1 = sampleRate / freq1 / 2;
  int duration1 = sampleRate / 10; // 0.1 seconds
  int currentSample = 0;
  int waveCounter = 0;
  int currentAmplitude = volume;

  while (currentSample < duration1)
  {
    int fifoSpace = *(audioPtr + 1);
    int leftSpace = (fifoSpace >> 24) & 0xFF;
    int rightSpace = (fifoSpace >> 16) & 0xFF;

    if (leftSpace > 0 && rightSpace > 0)
    {
      *(audioPtr + 2) = currentAmplitude;
      *(audioPtr + 3) = currentAmplitude;
      currentSample++;
      waveCounter++;

      if (waveCounter >= halfPeriod1)
      {
        currentAmplitude = -currentAmplitude;
        waveCounter = 0;
      }
    }
  }

  int pauseDuration = sampleRate / 20; // 0.05 seconds
  currentSample = 0;

  while (currentSample < pauseDuration)
  {
    int fifoSpace = *(audioPtr + 1);
    int leftSpace = (fifoSpace >> 24) & 0xFF;
    int rightSpace = (fifoSpace >> 16) & 0xFF;

    if (leftSpace > 0 && rightSpace > 0)
    {
      // Write 0 to the speakers to create silence
      *(audioPtr + 2) = 0;
      *(audioPtr + 3) = 0;
      currentSample++;
    }
  }

  int freq2 = 2637; // Slightly higher pitch!
  int halfPeriod2 = sampleRate / freq2 / 2;
  int duration2 = (sampleRate * 15) / 100; // 0.15 seconds
  currentSample = 0;
  waveCounter = 0;
  currentAmplitude = volume;

  while (currentSample < duration2)
  {
    int fifoSpace = *(audioPtr + 1);
    int leftSpace = (fifoSpace >> 24) & 0xFF;
    int rightSpace = (fifoSpace >> 16) & 0xFF;

    if (leftSpace > 0 && rightSpace > 0)
    {
      *(audioPtr + 2) = currentAmplitude;
      *(audioPtr + 3) = currentAmplitude;
      currentSample++;
      waveCounter++;

      if (waveCounter >= halfPeriod2)
      {
        currentAmplitude = -currentAmplitude;
        waveCounter = 0;
      }
    }
  }
}

// Read the switches 0-3
int readSwitches()
{
  volatile int *sw = (int *)SWITCH_BASE;
  return (*sw) & 0x1F;
}

// Show LEDs

void updateLEDs(int ledToLight)
{
  volatile int *led = (int *)LED_BASE;
  *led = ledToLight;
}

// Match the pins and display on led

void matchPins()
{
  int currentSwitches = readSwitches();
  matchedPins = 0;
  for (int pin = 0; pin < NUM_PINS; pin++)
  {
    int switchToggled = (currentSwitches >> pin) & 1;
    int patternBit = (targetPattern >> pin) & 1;
    if (switchToggled == patternBit)
    {
      matchedPins |= (1 << pin);
    }
  }

  //    updateLEDs(matchedPins);
}
void playFailSound()
{
  volatile int *audioPtr = (int *)AUDIO_BASE;
  int sampleRate = 8000;
  int volume = 0x00FFFFFF;

  int freq = 150; // Low, harsh buzz
  int halfPeriod = sampleRate / freq / 2;
  int duration = sampleRate / 3; // Play for 0.33 seconds
  int currentSample = 0;
  int waveCounter = 0;
  int currentAmplitude = volume;

  while (currentSample < duration)
  {
    int fifoSpace = *(audioPtr + 1);
    int leftSpace = (fifoSpace >> 24) & 0xFF;
    int rightSpace = (fifoSpace >> 16) & 0xFF;

    if (leftSpace > 0 && rightSpace > 0)
    {
      *(audioPtr + 2) = currentAmplitude;
      *(audioPtr + 3) = currentAmplitude;
      currentSample++;
      waveCounter++;

      if (waveCounter >= halfPeriod)
      {
        currentAmplitude = -currentAmplitude;
        waveCounter = 0;
      }
    }
  }
}

// Timer functions
void set_itimer(void)
{
  volatile int *timer_ptr = (int *)TIMER_BASE;

  int counterValue = 100000000; // 1 second at 100 MHz

  // Clear any previous interrupt first
  *timer_ptr = 0;

  *(timer_ptr + 0x2) = counterValue & 0xFFFF;         // low 16 bits
  *(timer_ptr + 0x3) = (counterValue >> 16) & 0xFFFF; // high 16 bits

  *(timer_ptr + 1) = 0x7; // start timer and enable START + CONT + ITO
}

void enable_interrupts(void)
{

  JP1_DIR = 0x00000000;  // all pins input
  JP1_EDGE = 0xFFFFFFFF; // clear stale edge capture
  JP1_IE = 0x00000001;   // interrupt on D0 only

  int mstatus_value, mtvec_value, mie_value;
  mstatus_value = 0b1000; // interrupt bit mask

  // Disable interrupts
  __asm__ volatile("csrc mstatus, %0" ::"r"(mstatus_value));
  mtvec_value = (int)&handler;
  __asm__ volatile("csrw mtvec, %0" ::"r"(mtvec_value));

  // Disable all interrupts that are currently enabled
  __asm__ volatile("csrr %0, mie" : "=r"(mie_value));
  __asm__ volatile("csrc mie, %0" ::"r"(mie_value));
  mie_value = (1 << 27) | (1 << 16); // Timer bit 16 and Rotary bit 27

  // Set interrupts enables
  __asm__ volatile("csrs mie, %0" ::"r"(mie_value));

  // Enable Nois V interrupts
  __asm__ volatile("csrs mstatus, %0" ::"r"(mstatus_value));
}

void handler(void)
{
  int mcause_value;
  __asm__ volatile("csrr %0, mcause" : "=r"(mcause_value));

  if (mcause_value == 0x80000010)
  { // interval timer interrupt
    itimer_ISR();
  }
  else if (mcause_value == JP1_MCAUSE)
  { // rotary interrupt
    rotary_ISR();
  }
}

void itimer_ISR(void)
{
  volatile int *timer_ptr = (int *)TIMER_BASE;

  // Clear timer interrupt
  *timer_ptr = 0;

  // Only count time during game state
  if (state == GAME_STATE && timerStarted == 1)
  {
    elapsedTime++;

    if (rotary_in_range){
      rotary_counter --;
      if (rotary_counter < 0) rotary_counter = 255;
    }
  }
}

void rotary_ISR(void)
{
  unsigned int pins = JP1_DATA;
  unsigned int B = (pins >> 1) & 1; // just get d1

  if (B == 1)
  { // clockwise
    rotary_counter++;
    if (rotary_counter > 255)
      rotary_counter = 0;
  }
  else
  {
    rotary_counter--;
    if (rotary_counter < 0)
      rotary_counter = 255;
  }

  *LEDR_ptr = rotary_counter;
  JP1_EDGE = 0xFFFFFFFF; // clear edge capture
}

void drawRotaryBar()
{
  int barX = 20;
  int barY = 218;
  int barW = 280;
  int barH = 5;

  drawRectangle(barX, barY, barW, barH, 0xAD55); // grey bar (should always be visible)

  // varibales used
  // rotary_counter
  // target_rotary_end
  // target_rotary_start

  int rotary_difference;

  if (rotary_counter > target_rotary_end)
  {
    rotary_difference = rotary_counter - target_rotary_end;
    rotary_in_range = false;
  }
  else if (rotary_counter < target_rotary_start)
  {
    rotary_difference = target_rotary_start - rotary_counter;
    rotary_in_range = false;
  }
  else
  { // if rotary is within the range
    rotary_difference = 0;
    rotary_in_range = true;
  }

  int percent = 100 - (rotary_difference * 100 / 128);
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;

  int drawnBarWidth = (percent * barW) / 100;

  short int bar_color = rotary_in_range ? 0x07E0 : 0xF800;

  if (drawnBarWidth > 0)
    drawRectangle(barX, barY, drawnBarWidth, barH, bar_color); // red bar
}

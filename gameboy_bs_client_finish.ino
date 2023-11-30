#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <WiFi.h>

// OLED Display Parameters
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SDA_PIN 5
#define SCL_PIN 6
#define OLED_RESET -1

// WiFi Parameters
const char* ssid = "Game_Boy_Battleship";
const char* password = "Hoa!Game!";
const char* host = "192.168.4.1"; // IP address of the server
WiFiClient client;

// OLED Display Initialization
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Keypad Parameters
#define O_1 20
#define O_2 10
#define O_3 0
#define I_1 7
#define I_2 8
#define I_3 9
bool isServerTurn = true; // Start with the server's turn, for instance
const byte ROWS = 3;
const byte COLS = 3;
byte rowPins[ROWS] = {O_1, O_2, O_3};
byte colPins[COLS] = {I_1, I_2, I_3};
char keys[ROWS][COLS] = {
  {'U', 'L', 'D'},
  {'R', 'X', 'Y'},
  {'A', 'B', 'Q'}
};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// Define game states
enum GameState {
  WAITING,
  PLAYING,
  RESULTING
};
GameState gameState = WAITING;

bool loss = false;

bool serverReady = false;
bool clientReady = false;

#define GRID_WIDTH 16
#define GRID_HEIGHT 8
#define CELL_WIDTH 8  // 128 pixels / 16 cells
#define CELL_HEIGHT 8 // 64 pixels / 8 cells

int cursorX = GRID_WIDTH / 2;   // Initial horizontal position of the cursor
int cursorY = GRID_HEIGHT / 2;  // Initial vertical position of the cursor

struct Ship {
    int x, y;
    bool isHorizontal;
    bool isAlive;
    int size;
    bool hasMoved; // Flag to indicate if the ship has moved this turn
    bool hasFired; // Flag to indicate if the ship has fired this turn
};

Ship serverShips[3]; // Server's big ships
Ship clientShips[3]; // Client's big ships
int selectedShipIndex = -1; // -1 means no ship is selected

void selectShip() {
    static int selectionCount = 0; // Counter for the number of selections made
    int aliveShips = 0; // Counter for the number of alive ships

    // Count the number of alive ships
    for (int i = 0; i < 3; ++i) {
        if (clientShips[i].isAlive) {
            aliveShips++;
        }
    }

    int initialIndex = selectedShipIndex;

    do {
        selectedShipIndex = (selectedShipIndex + 1) % 3;
        if (clientShips[selectedShipIndex].isAlive) {
            Ship& selectedShip = clientShips[selectedShipIndex];
            cursorX = selectedShip.x + selectedShip.size / 2;
            cursorY = selectedShip.y;
            selectionCount++;
            markSurroundingArea(selectedShip);
            selectedShip.hasMoved = false;
            selectedShip.hasFired = false;
            break;
        }
    } while (selectedShipIndex != initialIndex); // Avoid infinite loop

    // Check if all alive ships have been selected
    if (selectionCount > aliveShips) {
        // Reset selection count for the next turn
        selectionCount = 0;

        // Transition to the next turn (client's turn)
        isServerTurn = true;
        client.print("isServerTurn=true");
        //Serial.println("turn message send");
        selectedShipIndex = -1; // Deselect the ship
        display.clearDisplay(); // Clear the previous markings
        drawGrid(); // Redraw the grid
    }
}

void markSurroundingArea(Ship& ship) {
    int startX = max(0, ship.x - 2); // 2-grid margin on the left
    int endX = min(GRID_WIDTH - 1, ship.x + ship.size + 1); // Include ship length and margin on the right
    int startY = max(0, ship.y - 2); // 2-grid margin on the top
    int endY = min(GRID_HEIGHT - 1, ship.y + 2); // Extend one more row downward

    for (int x = startX; x <= endX; x++) {
        for (int y = startY; y <= endY; y++) {
            if (x >= ship.x && x < ship.x + ship.size && y == ship.y) {
                continue; // Skip the ship's own position
            }
            display.drawPixel(x * CELL_WIDTH + CELL_WIDTH / 2, y * CELL_HEIGHT + CELL_HEIGHT / 2, SSD1306_WHITE);
        }
    }
}

bool isWithinSurroundingArea(int x, int y, Ship& ship) {
    int startX = max(0, ship.x - 2);
    int endX = min(GRID_WIDTH - 1, ship.x + ship.size + 1);
    int startY = max(0, ship.y - 2);
    int endY = min(GRID_HEIGHT - 1, ship.y + 2);

    return x >= startX && x <= endX && y >= startY && y <= endY;
}

void moveShipToCursor() {
    if (selectedShipIndex != -1 && isWithinSurroundingArea(cursorX, cursorY, clientShips[selectedShipIndex])) {
        Ship& ship = clientShips[selectedShipIndex];
        ship.x = cursorX - ship.size / 2; // Center the ship on the cursor
        ship.y = cursorY;
        clientShips[selectedShipIndex].hasMoved = true;

        // Send the new ship position to the client
        client.print("MoveShip:" + String(selectedShipIndex) + "," + String(ship.x) + "," + String(ship.y));
    }
}

void fireAtPosition() {
    if (isWithinSurroundingArea(cursorX, cursorY, clientShips[selectedShipIndex])) {
        // Local check for a hit
        bool hit = false;
        clientShips[selectedShipIndex].hasFired = true;
        for (int i = 0; i < 3; i++) {
            if (isShipHit(cursorX, cursorY, serverShips[i])) {
                serverShips[i].isAlive = false; // Mark the ship as sunk
                hit = true;
                break;
            }
        }

        if (hit) {
            // Send a sink message to the client
            client.print("Sink:" + String(cursorX) + "," + String(cursorY));
        }
    }
}

bool isShipHit(int x, int y, Ship& ship) {
    if (!ship.isAlive) return false; // Skip already sunk ships

    for (int j = 0; j < ship.size; ++j) {
        int shipX = ship.isHorizontal ? ship.x + j : ship.x;
        int shipY = ship.isHorizontal ? ship.y : ship.y + j;
        if (x == shipX && y == shipY) {
            return true; // Attack hit the ship
        }
    }
    return false; // Attack missed
}

void processSinkMessage(const String& message) {
    // Remove the "Sink:" part
    String data = message.substring(strlen("Sink:"));
    int x = data.substring(0, data.indexOf(',')).toInt();
    int y = data.substring(data.indexOf(',') + 1).toInt();

    // Update the game state for the sunk ship
    for (int i = 0; i < 3; i++) {
        Ship& ship = clientShips[i]; // Replace with clientShips on the client side
        if (isShipHit(x, y, ship)) {
            ship.isAlive = false; // Mark the ship as sunk
            // Optionally, update display or game state here
            break;
        }
    }
}

void processMoveShipMessage(const String& message) {
    // Remove the "MoveShip:" part
    String data = message.substring(strlen("MoveShip:"));

    // Split the data by commas
    int firstCommaIndex = data.indexOf(',');
    int secondCommaIndex = data.lastIndexOf(',');

    int shipIndex = data.substring(0, firstCommaIndex).toInt();
    int newX = data.substring(firstCommaIndex + 1, secondCommaIndex).toInt();
    int newY = data.substring(secondCommaIndex + 1).toInt();

    // Update the ship's position
    if (shipIndex >= 0 && shipIndex < 3) { // Assuming there are 3 ships
        serverShips[shipIndex].x = newX;
        serverShips[shipIndex].y = newY;
    }
}

bool isCursorOnShip(int cursorX, int cursorY, Ship* ships, int numShips) {
    for (int i = 0; i < numShips; ++i) {
        Ship& ship = ships[i];
        if (!ship.isAlive) continue; // Skip sunk ships

        for (int j = 0; j < ship.size; ++j) {
            int shipX = ship.isHorizontal ? ship.x + j : ship.x;
            int shipY = ship.isHorizontal ? ship.y : ship.y + j;
            if (cursorX == shipX && cursorY == shipY) {
                return true; // Cursor is on an alive ship
            }
        }
    }
    return false; // Cursor is not on any alive ship
}

void drawGrid() {
    // Draw vertical lines
    for (int i = 0; i < GRID_WIDTH; i++) {
        int x = i * CELL_WIDTH;
        display.drawLine(x, 0, x, SCREEN_HEIGHT - 1, SSD1306_WHITE);
    }
    // Draw right edge line
    display.drawLine(SCREEN_WIDTH - 1, 0, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, SSD1306_WHITE);

    // Draw horizontal lines
    for (int j = 0; j < GRID_HEIGHT; j++) {
        int y = j * CELL_HEIGHT;
        display.drawLine(0, y, SCREEN_WIDTH - 1, y, SSD1306_WHITE);
    }
    // Draw bottom edge line
    display.drawLine(0, SCREEN_HEIGHT - 1, SCREEN_WIDTH - 1, SCREEN_HEIGHT - 1, SSD1306_WHITE);

  // Draw client's ships (for the client side)
    for (int i = 0; i < 3; ++i) drawShip(clientShips[i]);

    // Draw server's ships (for the server side, if they are sunk)
    for (int i = 0; i < 3; ++i) {
        if (!serverShips[i].isAlive) {
            drawShip(serverShips[i]);
        }
    }

    if (selectedShipIndex != -1) {
        // If a ship is selected, mark its surrounding area
        markSurroundingArea(clientShips[selectedShipIndex]); // Use clientShips for the client
    }

    int cursorPosX = cursorX * CELL_WIDTH + CELL_WIDTH / 2;
    int cursorPosY = cursorY * CELL_HEIGHT + CELL_HEIGHT / 2;
    uint16_t cursorColor = SSD1306_WHITE;

    // Check if the cursor is on one of the player's ships
    if (isCursorOnShip(cursorX, cursorY, clientShips, 3)) { // Replace 'clientShips' with 'serverShips' for the server code
        cursorColor = SSD1306_BLACK;
    }

    // Draw the cursor with the determined color
    display.drawLine(cursorPosX - 3, cursorPosY, cursorPosX + 3, cursorPosY, cursorColor); // Horizontal line
    display.drawLine(cursorPosX, cursorPosY - 3, cursorPosX, cursorPosY + 3, cursorColor); // Vertical line

    display.display();
}


void drawShip(const Ship& ship) {
    for (int i = 0; i < ship.size; ++i) {
        int x = (ship.x + i) * CELL_WIDTH;
        int y = ship.y * CELL_HEIGHT;
        if (!ship.isHorizontal) {
            x = ship.x * CELL_WIDTH;
            y = (ship.y + i) * CELL_HEIGHT;
        }

        // Draw the ship normally or as sunk
        if (ship.isAlive) {
            display.fillRect(x, y, CELL_WIDTH, CELL_HEIGHT, SSD1306_WHITE);
        } else {
            // Draw an 'X' for a sunk ship
            display.drawLine(x, y, x + CELL_WIDTH, y + CELL_HEIGHT, SSD1306_WHITE);
            display.drawLine(x, y + CELL_HEIGHT, x + CELL_WIDTH, y, SSD1306_WHITE);
        }
    }
}

void initializeServerShips() {
    for (int i = 0; i < 3; ++i) {
        serverShips[i].x = 0; // Start at left edge for the server
        serverShips[i].y = i * 2; // Space out the ships
        serverShips[i].isHorizontal = true; // Horizontal orientation
        serverShips[i].isAlive = true;
        serverShips[i].size = 3; // Big ship size (1x3)
        serverShips[i].hasMoved = false;
        serverShips[i].hasFired = false;
    }
}

void initializeClientShips() {
    for (int i = 0; i < 3; ++i) {
        clientShips[i].x = GRID_WIDTH - 3; // Start at right edge for the client
        clientShips[i].y = i * 2; // Space out the ships
        clientShips[i].isHorizontal = true; // Horizontal orientation
        clientShips[i].isAlive = true;
        clientShips[i].size = 3; // Big ship size (1x3)
        clientShips[i].hasMoved = false;
        clientShips[i].hasFired = false;     
    }
}

void connectToServer() {
  Serial.println("Attempting to connect to server...");
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print("Connexion au serveur...");
  display.display();

  while (!client.connect(host, 80)) {
    Serial.println("Failed to connect to server. Retrying...");
    delay(2000);
  }

  Serial.println("Connected to server.");
}

void setup() {
  Serial.begin(9600);
  Wire.begin(SDA_PIN, SCL_PIN);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  WiFi.begin(ssid, password);

  initializeClientShips();
  initializeServerShips();

  while (WiFi.status() != WL_CONNECTED) {
    display.clearDisplay();
    display.setCursor(0,0);
    display.print("Connexion WiFi...");
    display.display();
    delay(500);
  }

  connectToServer();
}

void loop() {
  switch (gameState) {
    case WAITING:
      handleWaitingState();
      break;
    case PLAYING:
      handlePlayingState();
      break;
    case RESULTING:
      handleResultingState();
      break;
  }

  delay(10);
}

void handleWaitingState() {
  display.clearDisplay();
  display.setCursor(0, 0);
  
  if (!client.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      display.setCursor(0, 56);
      display.print("Connecting to WiFi...");
      connectToServer();
    } else {
      display.setCursor(0, 56);
      display.print("WiFi connected, trying to find server");
    }
  } else {
    display.setCursor(0, 16);
    display.print("Server Connected.");
    
    if (!clientReady) {
      display.setCursor(0, 30);
      display.print("Press Y to ready");
      if (keypad.getKey() == 'Y') {
        clientReady = true;
        client.print("start"); // Send start message to server
      }
    } else if (!serverReady) {
      display.setCursor(0, 30);
      display.print("Wait for other player");
      if (client.available() > 0) {
        String message = client.readString();
        if (message == "start") {
          serverReady = true;
        }
      }
    }
    if (clientReady && serverReady) {
      gameState = PLAYING; // Transition to playing state
    }
  }
  display.display();
}

bool areAllShipsSunk(const Ship ships[]) {
    for (int i = 0; i < 3; ++i) {
        if (ships[i].isAlive) {
            return false;
        }
    }
    return true;
}

void processLoseMessage() {
    gameState = RESULTING;
}

void handlePlayingState() {
  if (client.available() > 0) {
      String message = client.readStringUntil('\n');
      if (message == "isServerTurn=false") {
          isServerTurn = false;
          //Serial.println("client turn");
      }
      else if (message.startsWith("MoveShip:")) {
        processMoveShipMessage(message);
      }
      else if (message.startsWith("Sink:")) {
          processSinkMessage(message);
      }
      else if (message == "Lose") {
          processLoseMessage();
      }
  }
  display.clearDisplay();
  if (areAllShipsSunk(clientShips)) {
      client.print("Lose");
      loss = true;
      gameState = RESULTING;
    }

    if (isServerTurn == false) {
        char key = keypad.getKey();
        switch (key) {
            case 'U': cursorY = max(0, cursorY - 1); break;
            case 'D': cursorY = min(GRID_HEIGHT - 1, cursorY + 1); break;
            case 'L': cursorX = max(0, cursorX - 1); break;
            case 'R': cursorX = min(GRID_WIDTH - 1, cursorX + 1); break;
            case 'X': selectShip(); break; // Ship selection
            case 'A': moveShipToCursor(); break;
            case 'B': fireAtPosition(); break;
            // Add additional controls for server's turn
        }
    }
    drawGrid(); // Redraw the grid with the updated cursor position
    // Additional logic to check for end of turn and switch isServerTurn
}

void handleResultingState() {
    display.clearDisplay();

    // Display "You Lost" or "You Won" in the center of the screen
    display.setTextSize(2); // Larger text size for better visibility
    display.setCursor(SCREEN_WIDTH / 4, SCREEN_HEIGHT / 4); // Adjust as needed for centering

    if (loss) {
        display.print("You Lost!");
    } else {
        display.print("You Won!");
    }

    // Display "Press Y to Restart" at the bottom of the screen
    display.setTextSize(1); // Reset text size for smaller prompt
    display.setCursor(0, SCREEN_HEIGHT - 10); // Position near the bottom
    display.print("Press Y to Restart");

    display.display();

    // Check for player input to restart the game
    char key = keypad.getKey();
    if (key == 'Y' || key == 'y') {
        resetGame();
    }
}

void resetGame() {
    // Reset all game variables and states for a new game
    initializeServerShips();
    initializeClientShips();
    gameState = WAITING;
    loss = false;
    selectedShipIndex = -1;
    serverReady = false;
    clientReady = false;
    // Further initialization as required
}

void updateDisplay(String input) {
  display.clearDisplay();
  display.setCursor(0, 0);
  display.print(input);
  display.display();
}

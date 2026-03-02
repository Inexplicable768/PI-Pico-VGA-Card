// Simple example of using the Uno Software Library to draw a character on the screen
// This interfaces with our custom VGA card firmware running on the Raspberry Pi Pico, which listens for commands over UART

void setup() {
  Serial.begin(115200);
  delay(1000);
}

void send_command(const char* cmd) {
  Serial.println(cmd);
  delay(100); // small delay to ensure command is processed
}
void loop() {

  // Draw blue rectangle
  send_command("FRECT 50 50 100 60 12");
  delay(1000);
  // Draw text
  send_command("TEXT 60 80 63 HELLO");
  delay(2000);

}
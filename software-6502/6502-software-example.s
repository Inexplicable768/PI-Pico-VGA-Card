; An example of interfacing with a 6502 CPU. This is a simple program that writes "Hello, World!" to the screen using the VGA card.



.section text


.section data
message:
    .ascii "Hello, World!\n"
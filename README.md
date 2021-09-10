# 3505_spreadsheet
Collaborative spreadsheet server project for CS3505 Spring 2021 semester

This was the final project in my CS3505 Software Practice II class at the UofU in Spring 2021. The project was to create a Google Sheets-like collaborative spreadsheet. The only code I am showcasing here is the server side, as I was primarily responsbile for that development. 

There was much help in the design of the spreadsheet.h and spreadsheet.cpp files from James Youngblood and help coding in all of the files from Ruben Conchas. 

This project was a great way to dive into a large, unfamiliar code library, namely boost asio. We examined many different possibilites for asynchronous tcp socket connections and boost asio seemed to be the most robust with adequate documentation. 

The server is based on a very specific communication protocol, which I will not share here to avoid giving away a solution to any future students. However, the backbone of the server is an asynchronous server than can connect, disconnect, send and receive messages. Because of this, it could be modified to match other protocols.

The design took many attemps with incremental development. If we were to do the project over again, I think I would design the server in a more modular way. As I said, the server would be modifiable to fit different needs, however it could have been broken apart more distinctly. There is some blending of the protocol with the general functioning of the server.

Tyler Liddell

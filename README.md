Implementation Details
This server program handles incoming HTTP connections in a multi-threaded manner. Here's a simplified breakdown of how the server operates:

Initialization: The server starts by parsing command-line arguments, where you can specify the number of threads for the server to use, and the port on which the server will listen for connections. If no thread count is provided, it defaults to 4 threads. The server then sets up the listener socket on the provided port and creates a queue to manage incoming connections.

Listening for Connections: The server's main thread continuously accepts incoming connections and pushes them onto the connection queue.

Worker Threads: The server spawns worker threads equal to the specified or default thread count. Each of these threads continuously pops connections off the queue and processes them.

Connection Processing: Each connection is represented by a conn_t structure which contains all the relevant information about the connection. The worker thread parses the HTTP request from the client, and depending on the request type, it proceeds to one of the request handlers - handle_get(), handle_put(), or handle_unsupported().

GET Request: In the case of a GET request, the requested resource's URI is extracted and a shared lock is acquired on the file before it's read. This lock ensures that other threads don't modify the file while it's being read. If the file doesn't exist or can't be read due to permissions issues, the appropriate HTTP response is sent back to the client.

PUT Request: For a PUT request, the requested resource's URI is extracted, and an exclusive lock is acquired on the file before it's written to. This lock ensures that no other thread can read or write to the file while this thread is modifying it. If the file already exists, the existing content is truncated before the new content is written.

Unsupported Requests: For all other request types, the server responds with a 501 Not Implemented status.

Logging: Every request processed by the server is logged with details including the method (GET or PUT), the requested URI, the request ID, and the HTTP status code of the response.

Cleanup: Once a request has been processed and a response has been sent, the worker thread closes the connection and returns to the queue to process the next connection.

This implementation ensures the program handles multiple connections efficiently, leveraging multiple threads to handle requests in parallel. Furthermore, it uses file locking to ensure data consistency when multiple threads are reading from and writing to files.

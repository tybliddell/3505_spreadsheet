#include <iostream>
#include <experimental/optional>
#include <boost/asio.hpp>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <boost/filesystem.hpp>

#include "spreadsheet.h"
using json = nlohmann::json;

using namespace std;

class session;
class error_catcher;

/* The server is in charge of listening for connecting clients, connecting them and
    managing them, and transferring data to and receiving data from the clients.
    The server hold the spreadsheets and interacts with them using methods declared in
    the spreadsheet.h file.
*/

/* Pool of current sessions. Accesses must be done in a thread safe manner
   using the session_mutex (lock) */
unordered_map<int, shared_ptr<session>> sessions;
/* Pool of current sessions by the spreadsheet. This is useful when sending
    messages to all users of a specific spreadsheet
    Accesses must be done in a thread safe manner using the session_mutex (lock) */

unordered_map<spreadsheet*, vector<shared_ptr<session>>> sessions_by_ss;
/* Pool of pending sessions that are currently in the handshake process. They are moved
    out of this pool when the handshake is complete.
    Accesses must be done in a thread safe manner using the session_mutex (lock) */
unordered_map<int, shared_ptr<session>> pending_sessions;
mutex session_mutex;

/* Global variable for id of the next client. Increments must be done in a thread
    safe manner using the id_mutex */
int curr_id = 1;
mutex id_mutex;

/* Map of the current spreadsheets. Any accesses or modifications to the spreadsheets must
    be done in a thread safe manner using the sheets_mutex */
unordered_map<string, spreadsheet*> sheets;

string get_ss_names();

/* A session represents a connection. Contains the socket, username, id, spreadsheet that
    connection is working on, as well as the buffer for that socket */
class session : public enable_shared_from_this<session>
{
    friend class client_listener;
    friend class error_catcher;
    boost::asio::ip::tcp::socket socket;
    boost::asio::streambuf streambuf;
    string username;
    string spreadsheet_name;
    string current_cell = " ";

public:
    int id;
    session(boost::asio::ip::tcp::socket&& socket)
    : socket(move(socket)), id(curr_id)
    {
        id_mutex.lock();
        curr_id++;
        id_mutex.unlock();
    }

    /* Client is in regular operation. Expected messages are editCell and selectCell
        This will process that message and then listen for another message */
    void start_reading()
    {
        boost::asio::async_read_until(socket, streambuf, '\n',

        // Lambda function for printing client's message
        [self = shared_from_this()] (boost::system::error_code error, size_t bytes_transferred)
        {

            // Remove client if error/disconnect
            if(error) {
                cout << "[update] Client " << self->id << " has disconnected" << endl;

                json disconnect_message;
                disconnect_message["messageType"] = "disconnected";
                disconnect_message["user"] = to_string(self->id);

                unordered_map<int, shared_ptr<session>>::iterator it;
                session_mutex.lock();
                sessions.erase(self->id);

                sheets[self->spreadsheet_name]->deselect_cell(self->current_cell, self->id); 
                //Find the client in sessions_by_ss and remove from vector
                vector<shared_ptr<session>> *ss_sessions = &sessions_by_ss[sheets[self->spreadsheet_name]];
                for(int i = 0; i < ss_sessions->size(); i++)
                    if(ss_sessions->at(i)->id == self->id) {
                        ss_sessions->erase(ss_sessions->begin() + i);
                        break;
                    }
                //Client disconnect message to send to all other clients
                string server_message = disconnect_message.dump() + "\n";
                for(it = sessions.begin(); it != sessions.end(); it++) {
                    try {
                        boost::asio::write(it->second->socket, boost::asio::buffer(server_message, server_message.size()));
                    }
                    catch (boost::wrapexcept<boost::system::system_error>& ex) {
                        cout << "[error] attempted to write to a broken pipe" << endl;
                    }
                    catch(...) {

                    }
                }
                session_mutex.unlock();
            }

            //Parse message from client. Can be a editCell, selectCell, undo, or revertCell request
            else {
                stringstream ss;
                ss << istream(&self->streambuf).rdbuf();
                string temp = ss.str();
                cout << "[update] Client " << self->id << " has sent: " << ss.str();

                try {
                    json client_message = json::parse(ss.str());

                    //Was an edit cell request
                    if(client_message["requestType"] == "editCell") {
                        //call edit cell
                        string cell_name = client_message["cellName"];
                        string desired_contents = client_message["contents"];
                        cout << "[update] Client " << self-> id << " (" << self->username << ") has requested to edit a cell. cellName: "
                         << cell_name << " to new contents " << desired_contents << endl;

                        spreadsheet *curr_sheet = sheets[self->spreadsheet_name];

                        (*curr_sheet->spreadsheet_mutex()).lock();
                        //The edit request was allowed. The client must have previously selected that same cell
                        if(curr_sheet->set_cell(cell_name, desired_contents, self->id)) {
                            json server_message;
                            server_message["messageType"] = "cellUpdated";
                            server_message["cellName"] = cell_name;
                            server_message["contents"] = desired_contents;

                            cout << "[update] Client " << self-> id << " (" << self->username << ") has edited a cell. cellName: "
                            << cell_name << " to new contents " << desired_contents << endl;
                            session_mutex.lock();
                            vector<shared_ptr<session>> clients = sessions_by_ss.at(sheets[self->spreadsheet_name]);

                            string message = server_message.dump() + "\n";
                            for(int i = 0; i < clients.size(); i++) {
                                try {
                                    boost::asio::write(clients[i]->socket, boost::asio::buffer(message, message.size()));
                                }
                                catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                    cout << "[error] attempted to write to a broken pipe" << endl;
                                }
                                catch(...) {

                                }
                            }
                            session_mutex.unlock();
                        }
                        //The edit request was not allowed for some reason
                        else {
                            json server_message;
                            server_message["messageType"] = "requestError";
                            server_message["cellName"] = cell_name;
                            server_message["message"] = "Unable to edit cell as desired";
                            session_mutex.lock();

                            cout << "[update] Client " << self-> id << " (" << self->username << ") was unable to edit a cell. cellName: "
                            << cell_name << " to new contents " << desired_contents << endl;

                            string message = server_message.dump() + "\n";
                            try {
                                boost::asio::write(self->socket, boost::asio::buffer(message, message.size()));
                            }
                            catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                cout << "[error] attempted to write to a broken pipe" << endl;
                            }
                            catch(...) {

                            }
                            session_mutex.unlock();
                        }
                        (*curr_sheet->spreadsheet_mutex()).unlock();
                    }

                    //Was a select cell request
                    else if(client_message["requestType"] == "selectCell") {
                        //call select cell
                        string cell_name = client_message["cellName"];
                        cout << "[update] Client " << self-> id << " (" << self->username << ") has requested to select a cell. cellName: " << cell_name << endl;

                        spreadsheet *curr_sheet = sheets[self->spreadsheet_name];

                        (*curr_sheet->spreadsheet_mutex()).lock();
                        //The select cell request was allowed
                        if(curr_sheet->select_cell(cell_name, self->username, self->id, self->current_cell)) {
                            self->current_cell = cell_name;
                            json server_message;
                            server_message["messageType"] = "cellSelected";
                            server_message["cellName"] = cell_name;
                            server_message["selector"] = self->id;
                            server_message["selectorName"] = self->username;

                            cout << "[update] Client " << self-> id << " (" << self->username << ") has selected a cell. cellName: " << cell_name << endl;
                            session_mutex.lock();
                            vector<shared_ptr<session>> clients = sessions_by_ss.at(sheets[self->spreadsheet_name]);

                            string message = server_message.dump() + "\n";
                            for(int i = 0; i < clients.size(); i++) {
                                try {
                                    boost::asio::write(clients[i]->socket, boost::asio::buffer(message, message.size()));
                                }
                                catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                    cout << "[error] attempted to write to a broken pipe" << endl;
                                }
                                catch(...) {

                                }
                            }
                            session_mutex.unlock();
                        }
                        //The select cell request was not allowed for some reason
                        else {
                            json server_message;
                            server_message["messageType"] = "requestError";
                            server_message["cellName"] = cell_name;
                            server_message["message"] = "Unable to select cell as desired";
                            cout << "[update] Client " << self-> id << " (" << self->username << ") was unable to select the cell. cellName: " << cell_name << endl;
                            session_mutex.lock();

                            string message = server_message.dump() + "\n";
                            try {
                                boost::asio::write(self->socket, boost::asio::buffer(message, message.size()));
                            }
                            catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                cout << "[error] attempted to write to a broken pipe" << endl;
                            }
                                catch(...) {

                                }
                            session_mutex.unlock();
                        }
                        (*curr_sheet->spreadsheet_mutex()).unlock();
                    }

                    //Was an undo request
                    else if(client_message["requestType"] == "undo") {
                        //call undo
                        cout << "[update] Client " << self-> id << " (" << self->username << ") has requested to undo" << endl;

                        spreadsheet *curr_sheet = sheets[self->spreadsheet_name];

                        (*curr_sheet->spreadsheet_mutex()).lock();
                        pair<string, string> new_pair = curr_sheet->undo();

                        //If the undo was a valid request
                        if(new_pair.first != "") {
                            string cell_name = new_pair.first;
                            string desired_contents = new_pair.second;
                            json server_message;
                            server_message["messageType"] = "cellUpdated";
                            server_message["cellName"] = cell_name;
                            server_message["contents"] = desired_contents;

                            cout << "[update] Client " << self-> id << " (" << self->username << ") has performed undo. Results: cellName: "
                            << cell_name << " to new contents " << desired_contents << endl;
                            session_mutex.lock();
                            vector<shared_ptr<session>> clients = sessions_by_ss.at(sheets[self->spreadsheet_name]);

                            string message = server_message.dump() + "\n";
                            for(int i = 0; i < clients.size(); i++) {
                                try {
                                    boost::asio::write(clients[i]->socket, boost::asio::buffer(message, message.size()));
                                }
                                catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                    cout << "[error] attempted to write to a broken pipe" << endl;
                                }
                                catch(...) {

                                }
                            }
                            session_mutex.unlock();

                        }

                        //The undo request was not allowed for some reason
                        else {
                            json server_message;
                            server_message["messageType"] = "requestError";
                            server_message["cellName"] = "N/A - Undo request";
                            server_message["message"] = "Unable to undo spreadsheet as desired";
                            session_mutex.lock();

                            cout << "[update] Client " << self-> id << " (" << self->username << ") was unable to undo" << endl;

                            string message = server_message.dump() + "\n";
                            try {
                                boost::asio::write(self->socket, boost::asio::buffer(message, message.size()));
                            }
                            catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                cout << "[error] attempted to write to a broken pipe" << endl;
                            }
                                catch(...) {

                                }
                            session_mutex.unlock();
                        }
                        (*curr_sheet->spreadsheet_mutex()).unlock();
                    }

                    //Was a revert request
                    else if(client_message["requestType"] == "revertCell") {
                        //call revert
                        cout << "[update] Client " << self-> id << " (" << self->username << ") has requested to revert a cell. cellName: " << client_message["cellName"] << endl;

                        spreadsheet *curr_sheet = sheets[self->spreadsheet_name];

                        (*curr_sheet->spreadsheet_mutex()).lock();

                        string cell_name = client_message["cellName"];
                        string new_contents;
                        //If the revert was a valid request
                        if(curr_sheet->revert_cell(cell_name, &new_contents)) {

                            json server_message;
                            server_message["messageType"] = "cellUpdated";
                            server_message["cellName"] = cell_name;
                            server_message["contents"] = new_contents;

                            cout << "[update] Client " << self-> id << " (" << self->username << ") has performed revert. Results: cellName: "
                            << cell_name << " to new contents " << new_contents << endl;
                            session_mutex.lock();
                            vector<shared_ptr<session>> clients = sessions_by_ss.at(sheets[self->spreadsheet_name]);

                            string message = server_message.dump() + "\n";
                            for(int i = 0; i < clients.size(); i++) {
                                try {
                                    boost::asio::write(clients[i]->socket, boost::asio::buffer(message, message.size()));
                                }
                                catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                    cout << "[error] attempted to write to a broken pipe" << endl;
                                }
                                catch(...) {

                                }
                            }
                            session_mutex.unlock();

                        }

                        //The revert request was not allowed for some reason
                        else {
                            json server_message;
                            server_message["messageType"] = "requestError";
                            server_message["cellName"] = cell_name;
                            server_message["message"] = "Unable to revert spreadsheet as desired";
                            session_mutex.lock();

                            cout << "[update] Client " << self-> id << " (" << self->username << ") was unable to revert a cell. cellName: "
                            << cell_name << endl;

                            string message = server_message.dump() + "\n";
                            try {
                                boost::asio::write(self->socket, boost::asio::buffer(message, message.size()));
                            }
                            catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                cout << "[error] attempted to write to a broken pipe" << endl;
                            }
                                catch(...) {

                            }
                            session_mutex.unlock();
                        }
                        (*curr_sheet->spreadsheet_mutex()).unlock();
                    }


                }
                catch(json::parse_error& ex) {
                    cout << "[error] Client has sent a bad message: " << ss.str() << endl;
                }
                self->start_reading();
            }
        });
    }

    /* Read the username from the client. This is the expected first message after recieving contact.
        Sends the spreadsheet names with a newline character following each of them and a newline character
        at the very end of the message. Proceed to receive their spreadsheet choice */
    void read_username() {
        boost::asio::async_read_until(socket, streambuf, '\n',

        // Lambda function for printing client's message
        [self = shared_from_this()] (boost::system::error_code error, size_t bytes_transferred)
        {
            // Remove client if error/disconnect
            if(error) {
                cout << "[update] Client " << self->id << " has disconnected" << endl;
                session_mutex.lock();
                pending_sessions.erase(self->id);
                session_mutex.unlock();
            }

            //Read username from client and send all spreasheet names
            else {
                stringstream ss;
                ss << istream(&self->streambuf).rdbuf();

                //store username, removing \n or \r
                string temp_string = ss.str();
                regex rem_newlines("\n+|\r+");
                self->username = regex_replace(temp_string, rem_newlines, "");
                cout << "[handshake] username received: " << self->username << endl;

                //send spreadsheets
                string ss_names = get_ss_names();
                try {
                    boost::asio::write(self->socket, boost::asio::buffer(ss_names, ss_names.size()));
                }
                catch (boost::wrapexcept<boost::system::system_error>& ex) {
                    cout << "[error] attempted to write to a broken pipe" << endl;
                }
                catch(...) {

                }

                // read which spreadsheet
                self->read_spreadsheet_choice();
            }
        });
    }

    /* Read the spreadsheet choice from the client and send the sheet as a series of cellUpdated messages back,
        followed by all of the currently selected cells on that spreadsheet, followed by the unique id of this client
        followed by a newline character. When sending this spreadsheet, no other clients may have edits go through to
        that spreadsheet */
    void read_spreadsheet_choice() {
        boost::asio::async_read_until(socket, streambuf, '\n',

        // Lambda function for printing client's message
        [self = shared_from_this()] (boost::system::error_code error, size_t bytes_transferred)
        {
            // Remove client if error/disconnect
            if(error) {
                cout << "[update] Client " << self->id << " has disconnected" << endl;
                session_mutex.lock();
                pending_sessions.erase(self->id);
                session_mutex.unlock();
            }

            else {
                stringstream ss;
                ss << istream(&self->streambuf).rdbuf();

                //Read spreadsheet name in. Remove any newline characters from name
                string temp_string = ss.str();
                regex rem_newlines("\n+|\r+");
                self->spreadsheet_name = regex_replace(temp_string, rem_newlines, "");
                cout << "[handshake] spreadsheet name received: " << self->spreadsheet_name << endl;

                //Send spreadsheet as cellUpdated messages and currently selected cells as cellSelected messages for clients
                //followed by newline character
                /* Sheet already exists on server. Send cell edits to get sheet in proper state,
                    followed by all selected cells, followed by the clients unique id */
                if(sheets.find(self->spreadsheet_name) != sheets.end()) {
                    sheets[self->spreadsheet_name]->spreadsheet_mutex()->lock();
                    //Retrieve all edits that must be made to create the current spreadsheet
                    vector<pair<string, string>> edits = sheets[self->spreadsheet_name]->all_cells();

                    //Send all edits to client
                    for(int i = 0; i < edits.size(); i++){
                        json message;
                        message["messageType"] = "cellUpdated";
                        message["cellName"] = edits.at(i).first;
                        message["contents"] = edits.at(i).second;


                        cout << endl;


                        try {
                            boost::asio::write(self->socket, boost::asio::buffer(message.dump() + "\n"));
                            cout << message << endl;
                        }
                        catch (boost::wrapexcept<boost::system::system_error>& ex) {
                            cout << "[error] attempted to write to a broken pipe" << endl;
                        }
                        catch(...) {

                        }


                        cout << endl;
                    }

                    //Retrive all selects on current spreadsheet
                    unordered_map<string, vector<pair<string, int> > > selects = sheets[self->spreadsheet_name]->all_selects();
                    unordered_map<string, vector<pair<string, int> > >::iterator it;
                    //For each cell that in the map
                    for(it = selects.begin(); it != selects.end(); it++) {
                        //Send all selects to client
                        json message;
                        message["messageType"] = "cellSelected";
                        message["cellName"] = it->first;
                        vector<pair<string, int> > curr_client = it->second;


                        cout << endl;


                        //For each client selecting that cell
                        for(int i = 0; i < curr_client.size(); i++) {
                            message["selector"] = to_string(curr_client.at(i).second);
                            message["selectorName"] = curr_client.at(i).first;

                            string server_message = message.dump() + "\n";
                            try {
                                boost::asio::write(self->socket, boost::asio::buffer(server_message, server_message.size()));
                                cout << server_message << endl;
                            }
                            catch (boost::wrapexcept<boost::system::system_error>& ex) {
                                cout << "[error] attempted to write to a broken pipe" << endl;
                            }
                            catch(...) {

                            }
                        }


                        cout << endl;
                    }

                    //Send client unique id as last message
                    string id_string = to_string(self->id) + "\n";
                    try {
                        boost::asio::write(self->socket, boost::asio::buffer(id_string, id_string.size()));
                    }
                    catch (boost::wrapexcept<boost::system::system_error>& ex) {
                        cout << "[error] attempted to write to a broken pipe" << endl;
                    }
                    catch(...) {

                    }


                }
                /* Sheet does not exist on the server. Create the new sheet and send client's unique
                    id */
                else {
                    //Create new spreadsheet
                    spreadsheet *new_sheet = new spreadsheet(self->spreadsheet_name);
                    sheets.insert(pair<string, spreadsheet*> (self->spreadsheet_name, new_sheet));
                    sheets[self->spreadsheet_name]->spreadsheet_mutex()->lock();

                    string id_string = to_string(self->id) + "\n";
                    try {
                        boost::asio::write(self->socket, boost::asio::buffer(id_string, id_string.size()));
                    }
                    catch (boost::wrapexcept<boost::system::system_error>& ex) {
                        cout << "[error] attempted to write to a broken pipe" << endl;
                    }
                    catch(...) {

                    }
                }

                //Add current user to both sessions_by_ss and pool of all sessions
                session_mutex.lock();
                //If they are the first user on that spreasheet, must create new vector for the list
                if(sessions_by_ss.find(sheets[self->spreadsheet_name]) == sessions_by_ss.end())
                    sessions_by_ss.insert(pair<spreadsheet*, vector<shared_ptr<session>>>(sheets[self->spreadsheet_name], vector<shared_ptr<session>>()));

                //Push user to that vector
                sessions_by_ss.at(sheets[self->spreadsheet_name]).push_back(self);

                //Remove from pending sessions and add to pool of sessions
                shared_ptr<session> curr_session = pending_sessions.at(self->id);
                pending_sessions.erase(self->id);
                sessions.insert(pair<int, shared_ptr<session>> (self->id, curr_session));
                session_mutex.unlock();
                sheets[self->spreadsheet_name]->spreadsheet_mutex()->unlock();
                self->start_reading();
            }
        });
    }
};

/*
* The client listener is the acceptor for new connections.
* Clients are accepted asynchronously, their message loop is begun,
* the client is added to the pending_sessions structure, and the listener
* begins listening again
*/
class client_listener
{
    boost::asio::io_context& io_context;
    boost::asio::ip::tcp::acceptor acceptor;
    experimental::optional<boost::asio::ip::tcp::socket> socket;

public:
    client_listener(boost::asio::io_context& io_context, uint16_t port)
    : io_context(io_context),
    acceptor  (io_context, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port))
    {
    }

    void async_accept()
    {
        socket.emplace(io_context);

        acceptor.async_accept(*socket,

        // Lambda function for accepting client
        [&] (boost::system::error_code error)
        {
            // New shared pointer to the same socket (instead of copying)
            shared_ptr<session> curr_session = make_shared<session>(move(*socket));

            // start client message loop
            curr_session->read_username();

            // Insert this shared pointer into sessions (add the client connection)
            session_mutex.lock();
            pending_sessions.insert(pair<int, shared_ptr<session>> (curr_session->id, curr_session));
            session_mutex.unlock();
            cout << "[update] Client has been accepted, id: " << curr_session->id << endl;

            // Get ready to accept the next connection
            async_accept();
        });
    }
};

/*
* Begins the listener for new clients
*/
void begin_listening(int port) {
    boost::asio::io_context io_context;
    client_listener srv(io_context, port);
    srv.async_accept();
    cout << "[status] Now listening for clients" << endl;
    io_context.run();
}

/*
* When the server is sent a SIGINT signal (ctrl-C) from the keyboard,
* the error is caught here. All spreadsheets are saved, the server notifies
* all clients, and the server shuts down.
*/
class error_catcher {
    public:
    static void exit_handler(sig_atomic_t s) {
        cout << endl << "[shutdown] server shutting down, saving current spreadsheets" << endl;

        json disconnect_message;
        disconnect_message["messageType"] = "serverError";
        disconnect_message["message"] = "Server has been signaled to shut down. Saving spreadsheets and ending all connections.";

        unordered_map<int, shared_ptr<session>>::iterator it;
        session_mutex.lock();

        string message = disconnect_message.dump() + "\n";
        for(it = sessions.begin(); it != sessions.end(); it++) {
            try{
                boost::asio::write(it->second->socket, boost::asio::buffer(message, message.size()));
            }
            catch (boost::wrapexcept<boost::system::system_error>& ex) {
                 cout << "[error] attempted to write to a broken pipe" << endl;
            }
            catch(...) {

            }
        }
        session_mutex.unlock();

        unordered_map<string, spreadsheet*>::iterator sheets_it;
        for(sheets_it = sheets.begin(); sheets_it != sheets.end(); sheets_it++) {
            string path = "./spreadsheets/" + sheets_it->first + ".sht";
            cout << "[shutdown] saving file " << sheets_it->first << " to " << path << endl;
            sheets_it->second->write_to_file(path);
        }
        exit(0);
    }
};

/*
* Read all .sht files and create spreadsheets out of them. This is called on server startup
*/
void read_sheets() {
    boost::filesystem::path p("./spreadsheets/");
    for (auto i = boost::filesystem::directory_iterator(p); i != boost::filesystem::directory_iterator(); i++)
    {
        if (!boost::filesystem::is_directory(i->path()))
        {
            try {
                spreadsheet *new_sheet = new spreadsheet("./spreadsheets/" + i->path().filename().string(), true);
                regex rem_period("\\..*$");
                sheets.insert(pair<string, spreadsheet*> (regex_replace(i->path().filename().string(), rem_period, ""), new_sheet));
                cout << "[startup] server reading file " << i->path().filename().string() << endl;
            }
            catch(...){
                cout << "[error] unable to read file " << i->path().filename().string() << ", that .sht may be corrupted or saved incorrectly" << endl;
            }
            
        }
        else
            continue;
    }
}

int main(int argc, char** argv)
{
    //Signal for server exit
    signal(SIGINT, error_catcher::exit_handler);

    //Ignore broken pipes -- broken client should not break server
    signal(SIGPIPE, SIG_IGN);

    /* read spreadsheets located in ./saved_sheets/ */
    read_sheets();

    /* begin listening for clients on other thread */
    begin_listening(1100);

    //while(1);

    return 0;
}

/* Helper functions */

/*
* Returns the names of all spreadsheets currently on the server, each seperated
* by a newline character, with another newline character at the end of the string
*/
string get_ss_names() {
    stringstream ss;
    unordered_map<string, spreadsheet*>::iterator it;
    for(it = sheets.begin(); it != sheets.end(); it++) {
        ss << it->first << "\n";
    }
    ss << "\n";
    return ss.str();
}

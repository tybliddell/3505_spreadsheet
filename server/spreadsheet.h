#include<string>
#include<vector>
#include<unordered_map>
#include<utility>
#include <iostream>
#include <fstream>
#include <mutex>
#include<regex>
#include <nlohmann/json.hpp>
#include <queue>
#include<regex>
#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>

using json = nlohmann::json;

using namespace std;

class spreadsheet {
  string name;

  mutex cell_history_mutex;
  unordered_map<string, vector<string> > cell_history;

  mutex general_history_mutex;
  vector<pair<string, string> > general_history;

  //Map of cell name to a vector of client_name and client id pair strings

  mutex selected_cells_mutex;
  unordered_map<string, vector<pair<string, int> > > selected_cells;
  mutex ss_mutex;

  public:
    spreadsheet(string);
    spreadsheet(string, bool); 

    bool set_cell(string, string, int);
    string get_cell(string);
    bool revert_cell(string, string *);
    vector<pair<string, string> > all_cells();
    bool select_cell(string, string, int, string);
    void deselect_cell(string, int);
    unordered_map<string, vector<pair<string, int> > > all_selects();
    pair<string, string> undo();
    void write_to_file(string);
    mutex* spreadsheet_mutex();
    

  private:
    static bool valid_cell_name(string);
    bool circular_depend(string, string);
    vector<string> find_depends(string);
    static bool valid_formula(string, string);
    vector<string> *get_history(string);
    vector<string> *get_history(string, string);
    static vector<string> get_tokens(string*);
};
#include"spreadsheet.h"

using json = nlohmann::json;


/**
  * spreadsheet empty constructor
  */
spreadsheet::spreadsheet(string name) : name(name) {}


/**
  * spreadsheet file constructor
  */
spreadsheet::spreadsheet(string path, bool differentiator) {
  ifstream txtFile(path); 
  
  string line;
  getline(txtFile, line);
  json new_name = json::parse(line);
  name = new_name["name"];

  cell_history_mutex.lock();
  cell_history = unordered_map<string, vector<string> >();

  while(getline(txtFile, line)) {
    json cell = json::parse(line);
    string cellName = cell["cellName"];
    //cell_history[cellName] = new vector<string>;
    cell_history[cellName].push_back(cell["contents"]);
  }

  cell_history_mutex.unlock();
  txtFile.close();
}


/**
  * set_cell
  */
bool spreadsheet::set_cell(string cell_name, string contents, int user_id) {
  bool correct_user = false;
  selected_cells_mutex.lock();
  for(int i = 0; i < selected_cells[cell_name].size(); i++)
    if(selected_cells[cell_name].at(i).second == user_id) {
      correct_user = true;
      break;
    }
  selected_cells_mutex.unlock();

  // If bad cell name or contents, refuse to edit
  if(contents.length() > 0 && contents.at(0) == '=' && !valid_formula(cell_name, contents))
    return false;
  if (!valid_cell_name(cell_name) || circular_depend(cell_name, contents) || !correct_user) {
    return false;
  }

  cell_history_mutex.lock();
  vector<string> *history = get_history(cell_name);
  
  // Otherwise add the last value it was to general history
  general_history_mutex.lock();
  general_history.push_back(make_pair(cell_name, history->back()));
  general_history_mutex.unlock();

  // Update cell 
  cell_history[cell_name].push_back(contents);
  cell_history_mutex.unlock();

  return true;
}


/**
  * get_cell
  */
string spreadsheet::get_cell(string cell_name) {

  // Return empty string on bad cell name
  if (!valid_cell_name(cell_name))
    return "";

  cell_history_mutex.lock();
  // Return most recent history, which is current contents
  string ret_val = get_history(cell_name)->back();
  cell_history_mutex.unlock();
  return ret_val;
}


/**
  * revert_cell
  */
bool spreadsheet::revert_cell(string cell_name, string * contents) {
  cell_history_mutex.lock();

  vector<string> * history = get_history(cell_name);

  // If bad cell name, no revert history, or circular dependency arises, refuse to edit
  if(!valid_cell_name(cell_name) || history->size() <= 1 || 
    circular_depend(cell_name, history->at(history->size() - 2))) {
    cell_history_mutex.unlock();
    return false;
  }

  // Revert to previous state, put on general history, set contents to new value
  //Previous content is the old content after the revert is complete
  string previousContent = history->back();
  history->pop_back();

  //history.push_back(history.at(history.size() - 2));
  general_history_mutex.lock();
  //Push previous contents onto general history so it can be undone
  general_history.push_back(make_pair(cell_name, previousContent));
  general_history_mutex.unlock();

  *contents = history->back();

  cell_history_mutex.unlock();
  return true;
}


/**
  * all_cells
  */
vector<pair<string, string>> spreadsheet::all_cells() {
  
  vector<pair<string, string> > cell_list;

  // Loop through all entries in the cell_history map, make pairs of cell_name to most recent value
  cell_history_mutex.lock();
  unordered_map<string, vector<string> >::iterator it = cell_history.begin();
  for (; it != cell_history.end(); it++) {
    cell_list.push_back(make_pair(it->first, it->second.back()));
  }
  cell_history_mutex.unlock();
  
  return cell_list;
}

/* 
 * selected cell is an unordered map mapping a cell name (string) to a vector of pairs
 * each pair has a client name (string) and id (int)
 *
 */
bool spreadsheet::select_cell(string cell_name, string client_name, int id, string old_cell_name) {
  if(!valid_cell_name(cell_name))
    return false;
  
  selected_cells_mutex.lock();

  if(old_cell_name != " ") {
    //Remove the old cell from the selected list
    for(int i = 0; i < selected_cells[old_cell_name].size(); i++)
      if(selected_cells[old_cell_name].at(i).second == id)
        selected_cells[old_cell_name].erase(selected_cells[old_cell_name].begin() + i);
  }

  //Select new cell
  selected_cells[cell_name].push_back(make_pair(client_name, id));
  
  selected_cells_mutex.unlock();

  return true;
}


void spreadsheet::deselect_cell(string cell_name, int client_id) {
  selected_cells_mutex.lock();
  if(cell_name != " ") {
    for(int i = 0; i < selected_cells[cell_name].size(); i++)
      if(selected_cells[cell_name].at(i).second == client_id)
        selected_cells[cell_name].erase(selected_cells[cell_name].begin() + i);
  }     
  selected_cells_mutex.unlock();
}

unordered_map<string, vector<pair<string, int> > > spreadsheet::all_selects() {
  return selected_cells;
}

/**
  * undo
  */
pair<string, string> spreadsheet::undo() {

  pair<string, string> edit("", "");

  if(general_history.size() >= 1) {
    general_history_mutex.lock();
    edit = general_history.back();
    general_history.pop_back();
    general_history_mutex.unlock();
  }
  
  return edit;
}


/**
  * write_to_file
  */
void spreadsheet::write_to_file(string path) {

  ofstream txtFile;
  
  txtFile.open(path, ofstream::trunc); 

  cell_history_mutex.lock();
  unordered_map<string, vector<string> >::iterator cell_iter = cell_history.begin();
  
  json new_name;
  new_name["name"] = name;

  txtFile << new_name.dump() << "\n";

  for (; cell_iter != cell_history.end(); cell_iter++) {
    json cell;
    cell["cellName"] = cell_iter->first;
    cell["contents"] = cell_iter->second.back(); 

    txtFile << cell.dump() << "\n";
  }

  cell_history_mutex.unlock();
  txtFile.close();
}


 

/**
  * valid_cell_name
  */
bool spreadsheet::valid_cell_name(string name) {
  regex expr("\\$?[a-zA-Z]+\\$?\\d+");
  //regex expr("[a-zA-Z_](?: [a-zA-Z_]|\\d)*");
  return regex_match(name, expr);
}


/**
  * circular_depend
  * implements a BFS
  */
bool spreadsheet::circular_depend(string cell_name, string contents) {
  unordered_map<string, bool> visited;
  queue<string> queue;
  visited[cell_name] = true;
  vector<string> dependencies = find_depends(contents);
  for(int i = 0; i < dependencies.size(); i++)
    queue.push(dependencies.at(i));

  for (;queue.size() > 0; queue.pop()) {
    string curr_cell = queue.front();
    if (visited[curr_cell])
      return true;

    visited[curr_cell] = true;
    
    cell_history_mutex.lock();
    dependencies = find_depends(get_history(curr_cell)->back());
    cell_history_mutex.unlock();

    for (int i = 0; i < dependencies.size(); i++)
      queue.push(dependencies.at(i));
  }

  return false;
}


/**
  * find_depends
  */
vector<string> spreadsheet::find_depends(string contents) {
  vector<string> tokens = get_tokens(& contents);
  vector<string> dependencies;
  
  for (int i = 0; i < tokens.size(); i++)
    if (valid_cell_name(tokens.at(i)))
      dependencies.push_back(tokens.at(i));

  return dependencies;
}


/**
  * valid_formula
  */
bool spreadsheet::valid_formula(string cell_name, string contents) {
  vector<string> tokens = get_tokens(&contents);

  int num_par = 0;

  //One token rule -- must be at least one token
  if(tokens.size() == 0)
    return false;

  //Starting token rule -- first token must be number, var, or opening parenth
  //TODO FIX WHEN I HAVE A BRAIN THAT WORKS AND IT'S NOT 1:00
  bool is_num;
  try {
    is_num = true;
    boost::lexical_cast<int>(tokens[0]);
  }
  catch(boost::bad_lexical_cast &) {
    is_num = false;
  }
  if(!(is_num || valid_cell_name(tokens[0]) || tokens[0] == "("))
    return false;
  
  //Ending token rule -- last token must be a number, var, or closing parenth
  //TODO FIX WHEN I HAVE A BRAIN THAT WORKS AND IT'S NOT 1:00
  try {
    is_num = true;
    boost::lexical_cast<int>(tokens[tokens.size()-1]);
  }
  catch(boost::bad_lexical_cast &) {
    is_num = false;
  }
  if(!(is_num || valid_cell_name(tokens[0]) || tokens[tokens.size()-1] == ")"))
    return false;
  
  string prev_token = "";

  for(int i = 0; i < tokens.size(); i++) {
    string curr_token = tokens[i];

    //Cannot have anything before if it is the first token
    if(i != 0) {
      // Parentheses/operator following rule -- Any token that immediately follows an opening
      //parenthesis or an operator must be either a number, a variable, or an opening parenthesis.
      if (prev_token =="(" || prev_token == "*" || prev_token == "/" || prev_token == "+" || prev_token == "-") {
        try {
          is_num = true;
          boost::lexical_cast<int>(curr_token);
        }
        catch(boost::bad_lexical_cast &) {
          is_num = false;
        }
        if (!(is_num || valid_cell_name(curr_token) || curr_token == "("))
          return false;
      }
      //Extra following rule -- Any token that immediately follows a number, a variable, or a 
      //closing parenthesis must be either an operator or a closing parenthesis.
      try {
          is_num = true;
          boost::lexical_cast<int>(prev_token);
      }
      catch(boost::bad_lexical_cast &) {
        is_num = false;
      }
      if (is_num || valid_cell_name(prev_token) || prev_token == ")")
        if (!(curr_token == "*" || curr_token == "/" || curr_token == "+" || curr_token == "-" || curr_token == ")"))
          return false;
    }
  
    //Check if operator 
    if(curr_token == "(" || curr_token == ")" || curr_token == "*" || curr_token == "/" || curr_token == "+" || curr_token == "-") {
      //Parentheses logic to assure balance
      if(curr_token == "(")
        num_par++;
      if(curr_token == ")")
        num_par--;
      //If unbalanced number of parentheses
      if(num_par < 0)
        return false;
    }

    prev_token = curr_token;    
  }
  //Unbalanced num of parentheses after entire formula parsing
  if(num_par != 0)
    return false;

  return true;
}


vector<string> spreadsheet::get_tokens(string *formula) {
  regex expression("(\\()|(\\))|([\\+\\-*\\/])|(\\$?[a-zA-Z]+\\$?\\d+)|((?:\\d+\\.\\d*|\\d*\\.\\d+|\\d+)(?:[eE][\\+-]?\\d+)?)|(\\s+)");
      
  regex_token_iterator<string::iterator> formula_iter(formula->begin(), formula->end(), expression);
  regex_token_iterator<string::iterator> iter_end;
  vector<string> tokens;
      
  while(formula_iter != iter_end) {
  string curr = *formula_iter++;
  if(curr != " ")
    tokens.push_back(curr);      
  }

  return tokens;
}

/**
  * get_history
  * Any calls to get_history and modification of the return must be done within
  * a cell_history_mutex locked zone.
  */
vector<string> *spreadsheet::get_history(string cell_name) {

  // If the cell is not in the history map, create it with empty state
  if (cell_history.find(cell_name) == cell_history.end()) {
    //cell_history[cell_name] = new vector<string>;
    cell_history[cell_name].push_back("");
  }

  return &cell_history[cell_name];
}

/**
  * Returns the history of this cell as a pointer to the vector.
  * Creates a history for the cell if it doesn't exist, setting the first data to
  * first_contents
  */
vector<string> *spreadsheet::get_history(string cell_name, string first_contents) {

  // If the cell is not in the history map, create it with empty state
  if (cell_history.find(cell_name) == cell_history.end()) {
    cell_history[cell_name] = vector<string>();
    cell_history[cell_name].push_back(first_contents);
  }

  return &cell_history[cell_name];
}

mutex* spreadsheet::spreadsheet_mutex() {
  return &ss_mutex;
}

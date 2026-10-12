#include "pch.h"
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>
#include <cstring>

#include "utils.h"

using namespace std;

unsigned long timeSinceTimepointMS(chrono::steady_clock::time_point timepoint)
{
    return (unsigned long)chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - timepoint).count();
}
unsigned long timeSinceTimepointUS(chrono::steady_clock::time_point timepoint)
{
    return (unsigned long)chrono::duration_cast<chrono::microseconds>(chrono::steady_clock::now() - timepoint).count();
}
chrono::steady_clock::time_point timeGetTimepointNow()
{
    return chrono::steady_clock::now();
}
unsigned long timeBetweenTimepointsUS(chrono::steady_clock::time_point timepoint1, chrono::steady_clock::time_point timepoint2)
{
    return (unsigned long)chrono::duration_cast<chrono::microseconds>(timepoint2 - timepoint1).count();
}

bool stringStartsWith(string haystack, string needle)
{
    return haystack.rfind(needle, 0) == 0;
}
string stringGetLastToken(string line)
{
    vector<string> tokens = stringSplit(line, ' ');
    return tokens.back();
}
string stringGetRestBehindFirstToken(string line)
{
    size_t firstSpace = line.find(' ');
    if (firstSpace == string::npos)
        return "";
    string rest = line.substr(firstSpace + 1);
    return rest;
}
string stringCutFirstToken(string& line)
{
    size_t firstSpace = line.find(' ');
    string token;
    if (firstSpace == string::npos)
    {
        token = line;
        line = "";
    }
    else
    {
        token = line.substr(0, firstSpace);
        line = line.substr(firstSpace + 1);
    }
    return token;
}
string stringCopyFirstToken(string line)
{
    size_t firstSpace = line.find(' ');
    if (firstSpace == string::npos)
        return line;
    return line.substr(0, firstSpace);
}
string stringToLower(string str)
{
    transform(str.begin(), str.end(), str.begin(), ::tolower);
    return str;
}
string stringToUpper(string str)
{
    transform(str.begin(), str.end(), str.begin(), ::toupper);
    return str;
}
vector<string> stringSplit(const string &line, char delimiter)
{
    vector<string> tokens;
    string token;
    istringstream tokenStream(line);
    while (getline(tokenStream, token, delimiter))
    {
        tokens.push_back(token);
    }
    return tokens;
}
bool stringToInt(string strval, int& result)
{
    try
    {
        size_t pos;
        result = stoi(strval, &pos);
        return pos == strval.length();
    }
    catch (...)
    {
        return false;
    }
}
bool stringReplace(string& haystack, const string& needle, const string& newneedle)
{
    size_t pos = haystack.find(needle);
    if (pos == string::npos)
        return false;
    haystack.replace(pos, needle.length(), newneedle);
    return true;
}
string stringIntToHex(const unsigned int i, unsigned int minLength)
{
    stringstream ss;
    ss << hex << uppercase << i;
    string str = ss.str();
    while (str.length() < minLength)
    {
        str.insert(0, 1, '0');
    }
    return str;
}
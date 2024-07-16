#include "Nickname.hpp"
#include "oatpp/data/stream/BufferStream.hpp"

const char* const Nickname::AVATARS[] = {
    "⚽", "⛄", "🏖", "⛵", "⛺", "🌀", "🌭", "🌲", "🌵", "🌶", "🌺", "🌻",
    "🍀", "🍄", "🍕", "🍭", "🎃", "🎾", "🐉", "🐌", "🐛", "🐠", "🐿", "🔮",
    "🗿", "🚌", "🤡", "🥐", "🦂", "🦄", "🦅", "🦉", "🦋", "🦍", "🦑", "🦁",
    "⛰",  "⛲", "🌂", "🎷", "🌋", "🌮", "🌴", "🍁", "🍆", "🍔", "🍡", "🍰",
    "🎅", "🎈", "🎭", "🎲", "🎸", "🎺", "🎻", "🏀", "🏂", "🏈", "🏉", "🏓",
    "🏹", "🏺", "🐈", "🐋", "🐚", "🐝", "🐞", "🐡", "🐢", "🐫"};

const char* const Nickname::ADJECTIVES[] = {
    "Attractive",  "Bald",       "Beautiful",  "Chubby",    "Clean",
    "Dazzling",    "Drab",       "Elegant",    "Fancy",     "Fit",
    "Flabby",      "Glamorous",  "Gorgeous",   "Handsome",  "Long",
    "Magnificent", "Muscular",   "Plain",      "Plump",     "Quaint",
    "Scruffy",     "Shapely",    "Short",      "Skinny",    "Stocky",
    "Ugly",        "Unkempt",    "Unsightly",  "Ashy",      "Black",
    "Blue",        "Gray",       "Green",      "Icy",       "Lemon",
    "Mango",       "Orange",     "Purple",     "Red",       "Salmon",
    "White",       "Yellow",     "Aggressive", "Agreeable", "Ambitious",
    "Brave",       "Calm",       "Delightful", "Eager",     "Faithful",
    "Gentle",      "Happy",      "Jolly",      "Kind",      "Lively",
    "Nice",        "Obedient",   "Polite",     "Proud",     "Silly",
    "Thankful",    "Victorious", "Witty",      "Wonderful", "Zealous",
    "Broad",       "Chubby",     "Crooked",    "Curved",    "Deep",
    "Flat",        "High",       "Hollow",     "Low",       "Narrow",
    "Refined",     "Round",      "Shallow",    "Skinny",    "Square",
    "Steep",       "Straight",   "Wide",       "Big",       "Colossal",
    "Fat",         "Gigantic",   "Great",      "Huge",      "Immense",
    "Large",       "Little",     "Mammoth",    "Massive",   "Microscopic",
    "Miniature",   "Petite",     "Puny",       "Scrawny",   "Short",
    "Small",       "Tall",       "Teeny",      "Tiny"};

const char* Nickname::NOUNS[] = {
    "Area",    "Book",     "Business", "Case",   "Child",  "Company",
    "Country", "Day",      "Eye",      "Fact",   "Family", "Government",
    "Group",   "Hand",     "Home",     "Job",    "Life",   "Lot",
    "Man",     "Money",    "Month",    "Mother", "Mr",     "Night",
    "Number",  "Part",     "People",   "Place",  "Point",  "Problem",
    "Program", "Question", "Right",    "Room",   "School", "State",
    "Story",   "Student",  "Study",    "System", "Thing",  "Time",
    "Water",   "Way",      "Week",     "Woman",  "Word",   "Work",
    "World",   "Year"};

thread_local std::mt19937 Nickname::RANDOM_GENERATOR(std::random_device{}());

oatpp::String Nickname::random() {
    std::uniform_int_distribution<v_int32> distroA(0, AVATARS_SIZE - 1);
    std::uniform_int_distribution<v_int32> distroJ(0, ADJECTIVES_SIZE - 1);
    std::uniform_int_distribution<v_int32> distroN(0, NOUNS_SIZE - 1);

    auto aIndex = distroA(RANDOM_GENERATOR);
    auto jIndex = distroJ(RANDOM_GENERATOR);
    auto nIndex = distroN(RANDOM_GENERATOR);

    oatpp::data::stream::BufferOutputStream stream;
    stream << AVATARS[aIndex] << " " << ADJECTIVES[jIndex] << " "
           << NOUNS[nIndex];

    return stream.toString();
}
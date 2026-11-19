// newsfeed.h - the news reader's engine (as the PicoVerse Audio Engine's
// pva_news.c): feeds (RSS 2.0, RSS 1.0/RDF, Atom), their stories, the full
// article (the feed's own content, or the paragraphs of the story's web page),
// My feeds (an OPML file) and the catalogue of feeds by topic.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct
{
    char   *title, *text, *link, *raw, *content;    // text: the summary; content: the feed's whole text
    int64_t t;                                       // UTC (0: no date, see raw)
    int     pos;                                     // its place in the feed
} nf_item;

enum { NF_IDLE, NF_BUSY, NF_DONE, NF_FAILED };

// a feed: in the background
void nf_fetch(const char *url);
int  nf_state(void);
unsigned nf_serial(void);                           // goes up with every nf_fetch
const char *nf_error(void);
const char *nf_feed_title(void);
// the stories (valid until the next nf_fetch); by_date: newest first, else the feed's order
nf_item *nf_items(int *count);
void nf_sort(bool by_date);

// the full article of a story: in the background
void nf_article(int i);
int  nf_article_state(void);                        // NF_*
const char *nf_article_text(void);                  // paragraphs, '\n' between them
size_t nf_article_bytes(void);                      // what came in so far

// "20:15" today, else "29 Oct"; long: "Thu 29 Oct 2026 21:15" (local time)
void nf_fmt_date(const nf_item *it, char *out, size_t n, bool lng);

// My feeds (MyFeeds.opml in the settings folder)
typedef struct { char name[120]; char url[600]; } nf_feed;
int  nf_favs(nf_feed **out);                        // the list (kept by the module)
bool nf_fav_add(const char *name, const char *url); // false: there already
void nf_fav_del(int i);
void nf_fav_move(int i, int dir);
bool nf_fav_has(const char *url);
int  nf_import_opml(const char *path);              // feeds added (-1: not readable)

// the catalogue
typedef struct { int topic; const char *code, *name, *url; } nf_cat;
extern const char *const nf_topics[];               // NULL at the end
extern const nf_cat nf_catalogue[];                 // name NULL at the end

// HTML to text (paragraphs '\n'); page: only <p> text, menus and the like left out
char *nf_html_text(const char *html, size_t n, bool page);

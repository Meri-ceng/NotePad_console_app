#include <ncurses.h>
#include <string>
#include <cstring>
#include <vector>
#include <stack>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <locale>
#include <wchar.h>

namespace fs = std::filesystem;

#define COLOR_NORMAL   1
#define COLOR_TOOLBOX  2
#define COLOR_STATUS   3
#define COLOR_SEARCH   4
#define COLOR_SELECT   5
#define COLOR_DIALOG   6
#define COLOR_TITLE    7

struct UndoRecord {
    std::vector<std::string> lines;
    int cursorRow, cursorCol;
};

struct EditorState {
    std::vector<std::string> lines;
    int cursorRow = 0, cursorCol = 0;
    int scrollRow = 0, scrollCol = 0;
    std::string filename;
    bool modified = false;

    bool selecting = false;
    int selStartRow = -1, selStartCol = -1;
    int selEndRow   = -1, selEndCol   = -1;

    std::string searchTerm;
    std::vector<std::pair<int,int>> matches;
    int matchIdx = -1;
    bool showMatches = false;

    std::string clipboard;
    std::stack<UndoRecord> undoStack;

    int termRows = 24, termCols = 80;
    int toolboxLines = 3;
    int statusLines  = 1;
    int editorRows() const { return termRows - toolboxLines - statusLines; }
};

EditorState E;
std::string statusMsg; // Tüm fonksiyonlar buna mesaj yazacak

void updateTermSize() { getmaxyx(stdscr, E.termRows, E.termCols); }

void pushUndo() {
    UndoRecord rec;
    rec.lines = E.lines;
    rec.cursorRow = E.cursorRow;
    rec.cursorCol = E.cursorCol;
    E.undoStack.push(rec);
}

void doUndo() {
    if (E.undoStack.empty()) return;
    UndoRecord rec = E.undoStack.top();
    E.undoStack.pop();
    E.lines = rec.lines;
    E.cursorRow = rec.cursorRow;
    E.cursorCol = rec.cursorCol;
    E.modified = true;
}

void clampCursor() {
    if (E.cursorRow < 0) E.cursorRow = 0;
    if (E.cursorRow >= (int)E.lines.size())
        E.cursorRow = (int)E.lines.size() - 1;
    if (E.cursorRow < 0) { E.lines.push_back(""); E.cursorRow = 0; }
    int lineLen = (int)E.lines[E.cursorRow].size();
    if (E.cursorCol < 0) E.cursorCol = 0;
    if (E.cursorCol > lineLen) E.cursorCol = lineLen;
}

void adjustScroll() {
    int edRows = E.editorRows();
    if (E.cursorRow < E.scrollRow) E.scrollRow = E.cursorRow;
    if (E.cursorRow >= E.scrollRow + edRows) E.scrollRow = E.cursorRow - edRows + 1;
    int numWidth = 5;
    int visibleCols = E.termCols - numWidth;
    if (E.cursorCol < E.scrollCol) E.scrollCol = E.cursorCol;
    if (E.cursorCol >= E.scrollCol + visibleCols) E.scrollCol = E.cursorCol - visibleCols + 1;
}

bool selectionOrdered(int &r1, int &c1, int &r2, int &c2) {
    r1 = E.selStartRow; c1 = E.selStartCol;
    r2 = E.selEndRow;   c2 = E.selEndCol;
    if (r1 > r2 || (r1 == r2 && c1 > c2)) { std::swap(r1,r2); std::swap(c1,c2); }
    return (r1 != -1);
}

bool inSelection(int row, int col) {
    if (E.selStartRow == -1) return false;
    int r1,c1,r2,c2;
    if (!selectionOrdered(r1,c1,r2,c2)) return false;
    if (row < r1 || row > r2) return false;
    if (row == r1 && col < c1) return false;
    if (row == r2 && col >= c2) return false;
    return true;
}

std::string getSelectedText() {
    int r1,c1,r2,c2;
    if (!selectionOrdered(r1,c1,r2,c2)) return "";
    std::string result;
    for (int r = r1; r <= r2; r++) {
        int start = (r==r1)?c1:0;
        int end   = (r==r2)?c2:(int)E.lines[r].size();
        if (r > r1) result += '\n';
        if (start < (int)E.lines[r].size())
            result += E.lines[r].substr(start, end-start);
    }
    return result;
}

void deleteSelected() {
    int r1,c1,r2,c2;
    if (!selectionOrdered(r1,c1,r2,c2)) return;
    std::string before = E.lines[r1].substr(0,c1);
    std::string after  = (c2<=(int)E.lines[r2].size()) ? E.lines[r2].substr(c2) : "";
    E.lines[r1] = before + after;
    E.lines.erase(E.lines.begin()+r1+1, E.lines.begin()+r2+1);
    E.cursorRow=r1; E.cursorCol=c1;
    E.selStartRow=E.selEndRow=-1;
    E.selStartCol=E.selEndCol=-1;
    E.selecting=false;
}

void findMatches(const std::string &term) {
    E.matches.clear(); E.matchIdx = -1;
    if (term.empty()) return;
    for (int r = 0; r < (int)E.lines.size(); r++) {
        size_t pos = 0;
        while ((pos = E.lines[r].find(term, pos)) != std::string::npos) {
            E.matches.push_back({r, (int)pos});
            pos += term.size();
        }
    }
    if (!E.matches.empty()) E.matchIdx = 0;
}

void drawToolbox() {
    attron(COLOR_PAIR(COLOR_TOOLBOX));
    for (int r=0;r<E.toolboxLines;r++) mvhline(r,0,' ',E.termCols);
    attroff(COLOR_PAIR(COLOR_TOOLBOX));

    attron(COLOR_PAIR(COLOR_TITLE)|A_BOLD);
    std::string title=" *** Mini Notepad++ *** ";
    mvprintw(0,(E.termCols-(int)title.size())/2,"%s",title.c_str());
    attroff(COLOR_PAIR(COLOR_TITLE)|A_BOLD);

    struct TI { const char* label; const char* key; };
    TI items[] = {
        {"Dosya Ac", "CTRL+O"}, {"Kaydet", "CTRL+S"}, {"F.Kaydet", "F2"},
        {"Metin Bul", "CTRL+F"}, {"Kelime Bul/Gez", "CTRL+G"}, {"Degistir", "CTRL+R"},
        {"Kopyala", "CTRL+C"}, {"Kes", "CTRL+X"}, {"Yapistir", "CTRL+V"},
        {"Geri Al", "CTRL+Z"}, {"Sec(Krk)", "SHIFT+YON"}, {"Sec(Kel)", "SHIFT+W"},
        {"Cikis", "ESC"}
    };
    int row=1,col=0;
    attron(COLOR_PAIR(COLOR_TOOLBOX));
    for (auto &it:items) {
        char buf[64];
        snprintf(buf,sizeof(buf)," %s[%s]",it.label,it.key);
        int len=(int)strlen(buf);
        if (col+len>=E.termCols){col=0;row++;if(row>=E.toolboxLines)break;}
        mvprintw(row,col,"%s",buf);
        col+=len;
    }
    attroff(COLOR_PAIR(COLOR_TOOLBOX));
}

void drawStatusBar(const std::string &msg="") {
    int row=E.termRows-1;
    attron(COLOR_PAIR(COLOR_STATUS));
    mvhline(row,0,' ',E.termCols);
    std::string left=" "+(E.filename.empty()?"[Yeni Dosya]":E.filename)+(E.modified?" *":"");
    std::string right="Sat:"+std::to_string(E.cursorRow+1)+" Sut:"+std::to_string(E.cursorCol+1)+" ";
    mvprintw(row,0,"%s",left.c_str());
    if (!msg.empty()) mvprintw(row,(int)left.size()+2,"%s",msg.c_str());
    mvprintw(row,E.termCols-(int)right.size(),"%s",right.c_str());
    attroff(COLOR_PAIR(COLOR_STATUS));
}

int getVisualWidth(const std::string& s, int cursorBytePos) {
    int width = 0;
    for (int i = 0; i < cursorBytePos && i < (int)s.size(); ) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) { i += 1; width += 1; }
        else if ((c & 0xE0) == 0xC0) { i += 2; width += 1; }
        else if ((c & 0xF0) == 0xE0) { i += 3; width += 1; }
        else if ((c & 0xF8) == 0xF0) { i += 4; width += 1; }
        else { i += 1; width += 1; }
    }
    return width;
}

void drawEditor() {
    int numWidth = 5;
    int edRows = E.editorRows();
    int startRow = E.toolboxLines;

    for (int screenRow = 0; screenRow < edRows; screenRow++) {
        int lineIdx = screenRow + E.scrollRow;
        int y = startRow + screenRow;

        // 1. SATIR NUMARALARI
        attron(COLOR_PAIR(COLOR_TOOLBOX));
        if (lineIdx < (int)E.lines.size()) mvprintw(y, 0, "%4d ", lineIdx + 1);
        else mvprintw(y, 0, "     ");
        attroff(COLOR_PAIR(COLOR_TOOLBOX));

        if (lineIdx >= (int)E.lines.size()) {
            attron(COLOR_PAIR(COLOR_NORMAL));
            mvhline(y, numWidth, ' ', E.termCols - numWidth);
            attroff(COLOR_PAIR(COLOR_NORMAL));
            continue;
        }

        const std::string &line = E.lines[lineIdx];
        int visibleCols = E.termCols - numWidth;
        int xOffset = numWidth;

        // --- KARAKTER BAZLI BOYAMA VE ÇİZİM ---
        // Satırı temizle
        attron(COLOR_PAIR(COLOR_NORMAL));
        mvhline(y, numWidth, ' ', visibleCols);

        int currentVisualWidth = 0;
        for (int i = 0; i < (int)line.size(); ) {
            // Karakterin kaç bayt olduğunu bul (UTF-8 desteği için)
            int charLen = 1;
            unsigned char c = (unsigned char)line[i];
            if (c >= 0xC0 && c <= 0xDF) charLen = 2;
            else if (c >= 0xE0 && c <= 0xEF) charLen = 3;
            else if (c >= 0xF0 && c <= 0xF7) charLen = 4;

            // Karakterin parçasını al
            std::string ch = line.substr(i, charLen);

            // Scroll sınırları içindeyse çiz
            if (currentVisualWidth >= E.scrollCol && (currentVisualWidth - E.scrollCol) < visibleCols) {
                bool sel = inSelection(lineIdx, i);
                bool inMatch = false;

                // Arama eşleşmesi kontrolü
                if (E.showMatches && !E.searchTerm.empty()) {
                    for (auto &m : E.matches) {
                        if (m.first == lineIdx && i >= m.second && i < m.second + (int)E.searchTerm.size()) {
                            inMatch = true;
                            break;
                        }
                    }
                }

                // RENK SEÇİMİ
                if (sel) attron(COLOR_PAIR(COLOR_SELECT)); // SARI (Seçim)
                else if (inMatch) attron(COLOR_PAIR(COLOR_SEARCH)); // MAVİ (Arama)
                else attron(COLOR_PAIR(COLOR_NORMAL));

                // Karakteri bas
                mvprintw(y, xOffset + (currentVisualWidth - E.scrollCol), "%s", ch.c_str());

                // Renkleri kapat
                if (sel) attroff(COLOR_PAIR(COLOR_SELECT));
                else if (inMatch) attroff(COLOR_PAIR(COLOR_SEARCH));
                else attroff(COLOR_PAIR(COLOR_NORMAL));
            }

            i += charLen;
            currentVisualWidth++;
        }
    }
}

void redraw(const std::string &msg="") {
    updateTermSize();
    clear();
    drawToolbox();
    drawEditor();
    drawStatusBar(msg);

    int numWidth = 5;
    int csr = E.cursorRow - E.scrollRow;
    int edRows = E.editorRows();

    if (csr >= 0 && csr < edRows) {
        int visualCol = getVisualWidth(E.lines[E.cursorRow], E.cursorCol);
        int visualScroll = getVisualWidth(E.lines[E.cursorRow], E.scrollCol);
        int finalX = numWidth + (visualCol - visualScroll);

        if (finalX >= numWidth && finalX < E.termCols) {
            curs_set(1);
            move(E.toolboxLines + csr, finalX);
        } else {
            curs_set(0);
        }
    } else {
        curs_set(0);
    }
    refresh();
}

std::string inputDialog(const std::string &prompt, const std::string &initial="") {
    int row=E.termRows/2;
    int width=std::max(55,(int)prompt.size()+12);
    int col=(E.termCols-width)/2;
    if (col<0) col=0;
    attron(COLOR_PAIR(COLOR_DIALOG)|A_BOLD);
    mvhline(row-1,col,' ',width);
    mvhline(row,col,' ',width);
    mvhline(row+1,col,' ',width);
    mvprintw(row-1,col+1,"%s",prompt.c_str());
    attroff(COLOR_PAIR(COLOR_DIALOG)|A_BOLD);
    std::string result=initial;
    auto redrawInput=[&](){
        attron(COLOR_PAIR(COLOR_DIALOG));
        mvprintw(row,col+1,"%-*s",(int)(width-2),result.c_str());
        move(row,col+1+(int)result.size());
        attroff(COLOR_PAIR(COLOR_DIALOG));
        refresh();
    };
    redrawInput();
    while (true) {
        int ch=getch();
        if (ch=='\n'||ch==KEY_ENTER) break;
        if (ch==27){result="";break;}
        if (ch==KEY_BACKSPACE||ch==127){if(!result.empty())result.pop_back();}
        else if (ch>=32&&ch<256) result+=(char)ch;
        redrawInput();
    }
    return result;
}

bool confirmDialog(const std::string &prompt) {
    int row=E.termRows/2;
    std::string full=" "+prompt+" (e/h) ";
    int col=(E.termCols-(int)full.size())/2;
    attron(COLOR_PAIR(COLOR_DIALOG)|A_BOLD);
    mvhline(row,col,' ',(int)full.size());
    mvprintw(row,col,"%s",full.c_str());
    attroff(COLOR_PAIR(COLOR_DIALOG)|A_BOLD);
    refresh();
    int ch=getch();
    return (ch=='e'||ch=='E'||ch=='y'||ch=='Y');
}

std::string fileManager(bool saveMode=false) {
    fs::path currentDir=fs::current_path();
    int selected=0;
    while (true) {
        std::vector<fs::directory_entry> entries;
        try { for (auto &e:fs::directory_iterator(currentDir)) entries.push_back(e); } catch(...) {}
        std::sort(entries.begin(),entries.end(),[](const fs::directory_entry &a,const fs::directory_entry &b){
            if (a.is_directory()!=b.is_directory()) return a.is_directory()>b.is_directory();
            return a.path().filename()<b.path().filename();
        });
        std::vector<std::string> items;
        items.push_back("[..] Ust dizin");
        for (auto &e:entries) {
            std::string name=e.path().filename().string();
            if (e.is_directory()) name="[D] "+name;
            else name="    "+name;
            items.push_back(name);
        }
        if (selected>=(int)items.size()) selected=0;
        clear();
        attron(COLOR_PAIR(COLOR_TITLE)|A_BOLD);
        std::string hdr=std::string(saveMode?" FARKLI KAYDET ":" DOSYA AC ")+" :: "+currentDir.string();
        mvprintw(0,0,"%-*s",E.termCols,hdr.c_str());
        attroff(COLOR_PAIR(COLOR_TITLE)|A_BOLD);
        attron(COLOR_PAIR(COLOR_STATUS));
        if (saveMode)
            mvprintw(1,0,"%-*s",E.termCols," YON:Gezin | ENTER:Sec | 'n':Yeni isim | ESC:Iptal");
        else
            mvprintw(1,0,"%-*s",E.termCols," YON:Gezin | ENTER:Ac/Gir | ESC:Iptal");
        attroff(COLOR_PAIR(COLOR_STATUS));
        int listStart=2, listEnd=E.termRows-2;
        int scrollOff=0;
        if (selected>=listEnd-listStart) scrollOff=selected-(listEnd-listStart)+1;
        for (int i=scrollOff;i<(int)items.size()&&i-scrollOff<listEnd-listStart;i++) {
            int y=listStart+i-scrollOff;
            bool isSel=(i==selected);
            if (isSel) attron(COLOR_PAIR(COLOR_SELECT)|A_BOLD);
            else attron(COLOR_PAIR(COLOR_NORMAL));
            mvprintw(y,0,"%-*s",E.termCols,(" "+items[i]).c_str());
            if (isSel) attroff(COLOR_PAIR(COLOR_SELECT)|A_BOLD);
            else attroff(COLOR_PAIR(COLOR_NORMAL));
        }
        refresh();
        int ch=getch();
        if (ch==KEY_UP) { selected--; if(selected<0)selected=0; }
        else if (ch==KEY_DOWN) { selected++; if(selected>=(int)items.size())selected=(int)items.size()-1; }
        else if (ch==27) return "";
        else if (ch=='\n'||ch==KEY_ENTER) {
            if (selected==0) { currentDir=currentDir.parent_path(); selected=0; }
            else {
                fs::path full=currentDir/entries[selected-1].path().filename();
                if (fs::is_directory(full)) { currentDir=full; selected=0; }
                else return full.string();
            }
        }
        else if (saveMode && ch=='n') {
            std::string name=inputDialog("Yeni dosya adi:");
            if (!name.empty()) return (currentDir/name).string();
        }
        else if (saveMode && ch>=32 && ch<127) {
            std::string init(1,(char)ch);
            std::string name=inputDialog("Dosya adi:",init);
            if (!name.empty()) return (currentDir/name).string();
        }
    }
}

void openFile() {
    std::string path=fileManager(false);
    if (path.empty()) return;
    std::ifstream f(path);
    if (!f.is_open()) { redraw("HATA: Dosya acilamadi!"); return; }
    E.lines.clear();
    std::string line;
    while (std::getline(f,line)) E.lines.push_back(line);
    if (E.lines.empty()) E.lines.push_back("");
    E.filename=path; E.modified=false;
    E.cursorRow=E.cursorCol=E.scrollRow=E.scrollCol=0;
    while (!E.undoStack.empty()) E.undoStack.pop();
}

bool saveFile(bool forceDialog=false) {
    if (E.filename.empty()||forceDialog) {
        std::string path=fileManager(true);
        if (path.empty()) return false;
        E.filename=path;
    }
    std::ofstream f(E.filename);
    if (!f.is_open()) { redraw("HATA: Kaydetme basarisiz!"); return false; }
    for (int i=0;i<(int)E.lines.size();i++) {
        f<<E.lines[i];
        if (i+1<(int)E.lines.size()) f<<'\n';
    }
    E.modified=false;
    return true;
}

void moveWordForward() {
    const std::string &line=E.lines[E.cursorRow];
    int col=E.cursorCol;
    while (col<(int)line.size()&&std::isalnum((unsigned char)line[col])) col++;
    while (col<(int)line.size()&&!std::isalnum((unsigned char)line[col])) col++;
    E.cursorCol=col;
}
void moveWordBackward() {
    const std::string &line=E.lines[E.cursorRow];
    int col=E.cursorCol;
    if (col>0) col--;
    while (col>0&&!std::isalnum((unsigned char)line[col])) col--;
    while (col>0&&std::isalnum((unsigned char)line[col-1])) col--;
    E.cursorCol=col;
}
void deleteWordBack() {
    if (E.cursorCol == 0) return; // Satır başındaysa silme
    pushUndo();
    
    const std::string &line = E.lines[E.cursorRow];
    int end = E.cursorCol;
    int start = end;

    // 1. Sondaki boşlukları atla
    while (start > 0 && line[start - 1] == ' ') start--;
    // 2. Kelimenin başına kadar git (Alfanümerik karakterleri geç)
    // Not: UTF-8 (Türkçe) karakterlerin ilk baytı 127'den büyüktür, onları da kelime sayalım
    while (start > 0 && (std::isalnum((unsigned char)line[start - 1]) || (unsigned char)line[start - 1] > 127)) {
        start--;
    }
    // 3. Eğer hiç karakter silinemediyse (örneğin sadece bir noktalama işareti varsa) en az 1 karakter sil
    if (start == end && start > 0) start--;

    E.lines[E.cursorRow].erase(start, end - start);
    E.cursorCol = start;
    E.modified = true;
}

void doSearch() { // CTRL + F için
    std::string term = inputDialog("Aranacak Metin:");
    if (term.empty()) { E.showMatches = false; return; }
    E.searchTerm = term;
    findMatches(term);
    E.showMatches = true; // Boyamayı açar
    if (!E.matches.empty()) {
        E.cursorRow = E.matches[0].first;
        E.cursorCol = E.matches[0].second;
        adjustScroll();
        statusMsg = "1/" + std::to_string(E.matches.size()) + " eslesme boyandi.";
    } else {
        E.showMatches = false;
        statusMsg = "Metin bulunamadi!";
    }
}

void doWordGo() { // CTRL + G için
    std::string term = inputDialog("Gidilecek Kelime:");
    if (term.empty()) return;
    findMatches(term);
    E.showMatches = false; // Navigasyonda boyama istenmiyorsa kapatılır
    if (!E.matches.empty()) {
        E.matchIdx = 0;
        E.cursorRow = E.matches[0].first;
        E.cursorCol = E.matches[0].second;
        adjustScroll();
        statusMsg = "1/" + std::to_string(E.matches.size()) + " (Yon tuslariyla gezin)";
    } else {
        statusMsg = "Kelime bulunamadi!";
    }
}

void goNextMatch() {
    if (E.matches.empty()) { statusMsg = "Aranan metin bulunamadi!"; return; }
    E.matchIdx = (E.matchIdx + 1) % (int)E.matches.size();
    E.cursorRow = E.matches[E.matchIdx].first;
    E.cursorCol = E.matches[E.matchIdx].second;
    adjustScroll();
    statusMsg = std::to_string(E.matchIdx + 1) + "/" + std::to_string(E.matches.size());
}

void goPrevMatch() {
    if (E.matches.empty()) { statusMsg = "Aranan metin bulunamadi!"; return; }
    E.matchIdx = (E.matchIdx - 1 + (int)E.matches.size()) % (int)E.matches.size();
    E.cursorRow = E.matches[E.matchIdx].first;
    E.cursorCol = E.matches[E.matchIdx].second;
    adjustScroll();
    statusMsg = std::to_string(E.matchIdx + 1) + "/" + std::to_string(E.matches.size());
}

void doReplace() {
    // 1. Önce değiştirilecek kelimeyi al
    std::string from = inputDialog("Degistirilecek kelime:");
    if (from.empty()) return; // Boş geçilirse çık

    // 2. Kelime metinde var mı kontrol et
    bool found = false;
    for (const auto &line : E.lines) {
        if (line.find(from) != std::string::npos) {
            found = true;
            break;
        }
    }

    if (!found) {
        statusMsg = "Degistirilecek kelime bulunamadi!";
        return; // Kelime yoksa yeni kelimeyi hiç sormadan çık
    }

    // 3. İptal ihtimaline karşı mevcut durumu yedekle (Yedekleme Noktası)
    std::vector<std::string> backupLines = E.lines;
    int backupRow = E.cursorRow;
    int backupCol = E.cursorCol;

    // 4. Yeni kelimeyi al
    std::string to = inputDialog("Yeni kelime (Iptal icin ESC):");
    
    // EĞER KULLANICI ESC'YE BASTIYSA (inputDialog "" dönerse)
    if (to.empty()) {
        E.lines = backupLines; // Metni eski haline döndür
        E.cursorRow = backupRow;
        E.cursorCol = backupCol;
        statusMsg = "Islem iptal edildi.";
        return;
    }

    // 5. Her şey yolundaysa değişikliği yapmadan önce Geri Al (Undo) yığınına ekle
    pushUndo();

    int count = 0;
    for (auto &line : E.lines) {
        size_t pos = 0;
        while ((pos = line.find(from, pos)) != std::string::npos) {
            line.replace(pos, from.size(), to);
            pos += to.size();
            count++;
        }
    }

    E.modified = true;
    statusMsg = std::to_string(count) + " eslesme degistirildi.";
}

void insertNewline() {
    pushUndo();
    if (E.selStartRow!=-1) deleteSelected();
    std::string rest=E.lines[E.cursorRow].substr(E.cursorCol);
    E.lines[E.cursorRow]=E.lines[E.cursorRow].substr(0,E.cursorCol);
    E.lines.insert(E.lines.begin()+E.cursorRow+1,rest);
    E.cursorRow++; E.cursorCol=0; E.modified=true;
}
void insertChar(int ch) {
    pushUndo();
    if (E.selStartRow!=-1) deleteSelected();
    E.lines[E.cursorRow].insert(E.cursorCol,1,(char)ch);
    E.cursorCol++; E.modified=true;
}
void doBackspace() {
    if (E.selStartRow!=-1){pushUndo();deleteSelected();E.modified=true;return;}
    if (E.cursorCol==0&&E.cursorRow==0) return;
    pushUndo();
    if (E.cursorCol==0) {
        int pl=(int)E.lines[E.cursorRow-1].size();
        E.lines[E.cursorRow-1]+=E.lines[E.cursorRow];
        E.lines.erase(E.lines.begin()+E.cursorRow);
        E.cursorRow--; E.cursorCol=pl;
    } else {
        E.lines[E.cursorRow].erase(E.cursorCol-1,1);
        E.cursorCol--;
    }
    E.modified=true;
}

void clearSelection() {
    E.selStartRow=E.selEndRow=-1;
    E.selStartCol=E.selEndCol=-1;
    E.selecting=false;
}

void mainLoop() {
    bool wordSelectMode = false; // Kelime seçim modunun durumunu takip eder

    while (true) {
        adjustScroll();
        redraw(statusMsg); 
        
        int ch = getch();
        statusMsg.clear(); 

        
        // --- 1. KELİME SEÇİM MODU TETİKLEYİCİSİ (SHIFT + W) ---
        if (ch == 'W') { 
            wordSelectMode = !wordSelectMode; // Modu aç/kapat
            if (wordSelectMode) {
                statusMsg = "KELIME SECIM MODU: AKTIF (Yon tuslariyla kelime secin)";
                if (E.selStartRow == -1) {
                    E.selStartRow = E.cursorRow;
                    E.selStartCol = E.cursorCol;
                }
            } else {
                statusMsg = "Kelime secim modu kapatildi.";
            }
            continue;
        }

        // --- 2. KELİME BAZLI SEÇİM (MOD AKTİFKEN YÖN TUŞLARI) ---
        if (wordSelectMode && (ch == KEY_LEFT || ch == KEY_RIGHT || ch == KEY_UP || ch == KEY_DOWN)) {
            if (ch == KEY_LEFT) moveWordBackward();
            else if (ch == KEY_RIGHT) moveWordForward();
            else if (ch == KEY_UP && E.cursorRow > 0) E.cursorRow--;
            else if (ch == KEY_DOWN && E.cursorRow + 1 < (int)E.lines.size()) E.cursorRow++;

            E.selEndRow = E.cursorRow;
            E.selEndCol = E.cursorCol;
            E.selecting = true;
            clampCursor();
            statusMsg = "Kelime bazli seciliyor... (Cikmak icin SHIFT+W veya ESC)";
            continue; 
        }

        // --- DOSYA İŞLEMLERİ ---
        if (ch == 15) { openFile(); continue; } // CTRL+O
        if (ch == 19) { saveFile(false); statusMsg = "Kaydedildi."; continue; } // CTRL+S
        if (ch == KEY_F(2)) { saveFile(true); statusMsg = "Farkli kaydedildi."; continue; } // F2

        // --- ARAMA VE NAVİGASYON ---
        if (ch == 6) { doSearch(); continue; } // CTRL+F
        
        if (ch == 7) { // CTRL+G
            std::string term = inputDialog("Gidilecek Kelime:");
            if (!term.empty()) {
                E.searchTerm = term; 
                findMatches(term);
                if (!E.matches.empty()) {
                    E.showMatches = true; 
                    E.matchIdx = 0;
                    E.cursorRow = E.matches[0].first;
                    E.cursorCol = E.matches[0].second;
                    adjustScroll();
                    statusMsg = "1/" + std::to_string(E.matches.size()) + " (Yon tuslariyla gezebilirsiniz)";
                } else {
                    E.showMatches = false;
                    statusMsg = "'" + term + "' bulunamadi!";
                }
            }
            continue; 
        }

        // --- DEĞİŞTİR ---
        if (ch == 18) { doReplace(); continue; } // CTRL+R

        // --- DÜZENLEME (KOPYALA / KES / YAPISTIR) ---
        if (ch == 3) { E.clipboard = getSelectedText(); statusMsg = "Kopyalandi."; continue; }
        if (ch == 24) { // CTRL+X
            E.clipboard = getSelectedText();
            if (!E.clipboard.empty()) { pushUndo(); deleteSelected(); E.modified = true; }
            statusMsg = "Kesildi."; continue;
        }
        if (ch == 22) { // CTRL+V
            if (!E.clipboard.empty()) {
                pushUndo();
                if (E.selStartRow != -1) deleteSelected();
                std::istringstream ss(E.clipboard);
                std::string token; bool first = true;
                while (std::getline(ss, token)) {
                    if (!first) insertNewline(); else first = false;
                    for (char c : token) {
                        E.lines[E.cursorRow].insert(E.cursorCol, 1, c);
                        E.cursorCol++;
                    }
                }
                E.modified = true;
            }
            E.showMatches = false; E.matchIdx = -1;
            statusMsg = "Yapistirildi."; continue;
        }

        // --- GERİ AL (UNDO) ---
        if (ch == 26) { doUndo(); E.showMatches = false; E.matchIdx = -1; statusMsg = "Geri alindi."; continue; }

        // --- ÇIKIŞ VE İPTAL (ESC) ---
        if (ch == 27) {
            wordSelectMode = false;
            if (E.selStartRow != -1) { clearSelection(); statusMsg = "Secim iptal edildi."; continue; }
            if (E.showMatches || E.matchIdx != -1) { 
                E.showMatches = false; 
                E.matchIdx = -1; 
                E.matches.clear(); 
                statusMsg = "Arama ve navigasyon temizlendi."; 
                continue; 
            }
            if (E.modified) {
                if (confirmDialog("Kaydedilmemis degisiklik. Kaydet?")) saveFile(false);
            }
            break;
        }

        // --- KARAKTER BAZLI SEÇİM (SHIFT + YÖN TUŞLARI) ---
        else if (ch == KEY_SLEFT || ch == KEY_SRIGHT || ch == KEY_SR || ch == KEY_SF) {
            wordSelectMode = false; 
            if (E.selStartRow == -1) { 
                E.selStartRow = E.cursorRow; 
                E.selStartCol = E.cursorCol; 
            }
            
            if (ch == KEY_SLEFT) {
                if (E.cursorCol > 0) E.cursorCol--;
                else if (E.cursorRow > 0) { E.cursorRow--; E.cursorCol = E.lines[E.cursorRow].size(); }
            } 
            else if (ch == KEY_SRIGHT) {
                if (E.cursorCol < (int)E.lines[E.cursorRow].size()) E.cursorCol++;
                else if (E.cursorRow + 1 < (int)E.lines.size()) { E.cursorRow++; E.cursorCol = 0; }
            }
            else if (ch == KEY_SR) { if (E.cursorRow > 0) E.cursorRow--; } 
            else if (ch == KEY_SF) { if (E.cursorRow + 1 < (int)E.lines.size()) E.cursorRow++; }

            E.selEndRow = E.cursorRow; E.selEndCol = E.cursorCol;
            E.selecting = true;
            clampCursor();
            continue; 
        }

        // --- STANDART YÖN TUŞLARI VE NAVİGASYON ---
        else if (ch == KEY_RIGHT) {
            wordSelectMode = false;
            if (E.matchIdx != -1 && !E.matches.empty()) {
                E.matchIdx = (E.matchIdx + 1) % (int)E.matches.size();
                E.cursorRow = E.matches[E.matchIdx].first;
                E.cursorCol = E.matches[E.matchIdx].second;
                statusMsg = std::to_string(E.matchIdx + 1) + "/" + std::to_string(E.matches.size());
                continue;
            }
            E.showMatches = false; E.matchIdx = -1; clearSelection();
            if (E.cursorCol < (int)E.lines[E.cursorRow].size()) E.cursorCol++;
            else if (E.cursorRow + 1 < (int)E.lines.size()) { E.cursorRow++; E.cursorCol = 0; }
        }
        else if (ch == KEY_LEFT) {
            wordSelectMode = false;
            if (E.matchIdx != -1 && !E.matches.empty()) {
                E.matchIdx = (E.matchIdx - 1 + (int)E.matches.size()) % (int)E.matches.size();
                E.cursorRow = E.matches[E.matchIdx].first;
                E.cursorCol = E.matches[E.matchIdx].second;
                statusMsg = std::to_string(E.matchIdx + 1) + "/" + std::to_string(E.matches.size());
                continue;
            }
            E.showMatches = false; E.matchIdx = -1; clearSelection();
            if (E.cursorCol > 0) E.cursorCol--;
            else if (E.cursorRow > 0) { E.cursorRow--; E.cursorCol = (int)E.lines[E.cursorRow].size(); }
        }
        else if (ch == KEY_UP || ch == KEY_DOWN) {
            wordSelectMode = false;
            E.showMatches = false; E.matchIdx = -1; clearSelection();
            if (ch == KEY_UP && E.cursorRow > 0) E.cursorRow--;
            else if (ch == KEY_DOWN && E.cursorRow + 1 < (int)E.lines.size()) E.cursorRow++;
        }
        else if (ch == KEY_HOME || ch == KEY_END || ch == KEY_PPAGE || ch == KEY_NPAGE) {
            wordSelectMode = false;
            E.showMatches = false; E.matchIdx = -1; clearSelection();
            if (ch == KEY_HOME) E.cursorCol = 0;
            else if (ch == KEY_END) E.cursorCol = (int)E.lines[E.cursorRow].size();
            else if (ch == KEY_PPAGE) E.cursorRow = std::max(0, E.cursorRow - E.editorRows());
            else E.cursorRow = std::min((int)E.lines.size() - 1, E.cursorRow + E.editorRows());
        }

        // --- SİLME VE YAZMA ---
        else if (ch == 8) { wordSelectMode = false; E.showMatches = false; E.matchIdx = -1; deleteWordBack(); continue; }
        else if (ch == KEY_BACKSPACE || ch == 127) { wordSelectMode = false; E.showMatches = false; E.matchIdx = -1; doBackspace(); }
        else if (ch == KEY_DC) { 
            wordSelectMode = false; E.showMatches = false; E.matchIdx = -1;
            if (E.cursorCol < (int)E.lines[E.cursorRow].size()) {
                pushUndo(); E.lines[E.cursorRow].erase(E.cursorCol, 1); E.modified = true;
            } else if (E.cursorRow + 1 < (int)E.lines.size()) {
                pushUndo(); E.lines[E.cursorRow] += E.lines[E.cursorRow + 1];
                E.lines.erase(E.lines.begin() + E.cursorRow + 1); E.modified = true;
            }
        }
        else if (ch == '\n' || ch == KEY_ENTER) { wordSelectMode = false; E.showMatches = false; E.matchIdx = -1; insertNewline(); }
        else if (ch >= 32 && ch < 256) { wordSelectMode = false; E.showMatches = false; E.matchIdx = -1; insertChar(ch); }

        clampCursor();
    }
}

int main(int argc, char *argv[]) {
    setlocale(LC_ALL,"");
    initscr(); raw(); noecho();
    keypad(stdscr,TRUE); curs_set(1);
    start_color(); use_default_colors();
    init_pair(COLOR_NORMAL,  COLOR_WHITE,  COLOR_BLACK);
    init_pair(COLOR_TOOLBOX, COLOR_BLACK,  COLOR_CYAN);
    init_pair(COLOR_STATUS,  COLOR_BLACK,  COLOR_WHITE);
    init_pair(COLOR_SEARCH,  COLOR_WHITE,  COLOR_BLUE);
    init_pair(COLOR_SELECT,  COLOR_BLACK,  COLOR_YELLOW);
    init_pair(COLOR_DIALOG,  COLOR_WHITE,  COLOR_MAGENTA);
    init_pair(COLOR_TITLE,   COLOR_BLACK,  COLOR_GREEN);

    E.lines.push_back("");

    if (argc>1) {
        std::ifstream f(argv[1]);
        if (f.is_open()) {
            E.lines.clear();
            std::string line;
            while (std::getline(f,line)) E.lines.push_back(line);
            if (E.lines.empty()) E.lines.push_back("");
            E.filename=argv[1];
        }
    }
    updateTermSize();
    mainLoop();
    endwin();
    return 0;
}


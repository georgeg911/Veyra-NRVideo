#include "veyra/ui/PlaybackServer.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int delivered=0, failed=0, checks=0;
    QString last;
    int lastQuality=-1;
    QList<veyra::ui::PlaybackServer::Part> lastParts;
    veyra::ui::PlaybackServer server([&](const QString& url,int quality,
                                        const QList<veyra::ui::PlaybackServer::Part>& parts){
        ++delivered;last=url;lastQuality=quality;lastParts=parts;});
    if(!server.listen(0)) return 1;
    const QByteArray host="127.0.0.1:"+QByteArray::number(server.serverPort());
    const QByteArray origin="chrome-extension://"+QByteArray(32,'a');
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failed;std::cerr<<name<<"\n";}};
    auto request=[&](QByteArray bytes,bool fragment=false){
        QTcpSocket socket;QEventLoop loop;QByteArray reply;
        QObject::connect(&socket,&QTcpSocket::connected,&loop,[&]{
            if(fragment){socket.write(bytes.left(17));QTimer::singleShot(5,&socket,[&]{socket.write(bytes.mid(17));});}
            else socket.write(bytes);
        });
        QObject::connect(&socket,&QTcpSocket::readyRead,&loop,[&]{reply+=socket.readAll();});
        QObject::connect(&socket,&QTcpSocket::disconnected,&loop,&QEventLoop::quit);
        QTimer::singleShot(2000,&loop,&QEventLoop::quit);
        socket.connectToHost("127.0.0.1",server.serverPort());loop.exec();
        app.processEvents();return reply;
    };
    auto post=[&](QByteArray body,QByteArray extra=QByteArray()){
        return QByteArray("POST /play HTTP/1.1\r\nHost: ")+host+"\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n"+extra+"\r\n"+body;
    };
    check(request("GET /ping HTTP/1.1\r\nHost: "+host+"\r\n\r\n").startsWith("HTTP/1.1 200"),"ping");
    auto body=QJsonDocument(QJsonObject{{"url","https://example.com/movie.mp4?a=1"}}).toJson(QJsonDocument::Compact);
    auto reply=request(post(body,"Origin: "+origin+"\r\n"),true);
    check(reply.startsWith("HTTP/1.1 200"),"fragmented extension request");
    check(reply.contains("Access-Control-Allow-Origin: "+origin),"scoped CORS");
    check(!reply.contains("Allow-Origin: *"),"no wildcard CORS");
    check(delivered==1&&last=="https://example.com/movie.mp4?a=1","dispatch exactly once");
    check(request(post(body,"Origin: https://evil.example\r\n")).startsWith("HTTP/1.1 403"),"web origin denied");
    check(request(post(body,"Origin: null\r\n")).startsWith("HTTP/1.1 403"),"null origin denied");
    check(request("GET /ping HTTP/1.1\r\nHost: evil.example\r\n\r\n").startsWith("HTTP/1.1 403"),"DNS rebinding denied");
    check(request(post(body,"Host: "+host+"\r\n")).startsWith("HTTP/1.1 400"),"duplicate host");
    check(request(post(body,"Content-Length: 0\r\n")).startsWith("HTTP/1.1 400"),"duplicate length");
    check(request("POST /play HTTP/1.1\r\nHost: "+host+"\r\n\r\n").startsWith("HTTP/1.1 411"),"missing length");
    // maxBody 现在是 256 KB（要给多P 分集留空间），所以这里用一个明确超过它的长度。
    // maxBody is 256 KB now (multi-part payloads need the room), so use a length clearly
    // past it.
    check(request("POST /play HTTP/1.1\r\nHost: "+host+"\r\nContent-Length: 262145\r\n\r\n").startsWith("HTTP/1.1 413"),"bounded body");
    check(request("GET /ping HTTP/1.1\r\nHost: "+host+"\r\nX: "+QByteArray(9000,'a')).startsWith("HTTP/1.1 413"),"bounded headers");
    check(request(post(body,"Transfer-Encoding: chunked\r\n")).startsWith("HTTP/1.1 400"),"chunked denied");
    check(request(post("{bad")).startsWith("HTTP/1.1 400"),"invalid JSON");
    check(request(post(R"({"url":"file:///C:/Windows/win.ini"})")).startsWith("HTTP/1.1 400"),"file URL denied");
    check(request(post(R"({"url":"https://name:secret@example.com"})")).startsWith("HTTP/1.1 400"),"URL credentials denied");
    check(request(post(R"({"url":"https://example.com","cookie":"bad\r\nInjected: yes"})")).startsWith("HTTP/1.1 400"),"cookie injection denied");
    // A browser that is signed in to Bilibili sends its session along; that must
    // not invalidate the request (Veyra keeps its own session, but the extension
    // always attaches the browser's).
    check(request(post(R"({"url":"https://example.com","cookie":"session=test"})")).startsWith("HTTP/1.1 200"),"browser cookie accepted");
    check(delivered==2,"rejections never dispatch");
    check(request(post(R"({"url":"https://example.com/live.m3u8","quality":80,"cookie":""})")).startsWith("HTTP/1.1 200"),"legacy direct URL without credentials");
    check(delivered==3&&last=="https://example.com/live.m3u8","legacy direct URL delivered");
    check(lastQuality==80,"quality reaches the handler");
    check(request(post(R"({"url":"https://example.com/b.mp4","quality":-1})")).startsWith("HTTP/1.1 200"),"page quality sentinel accepted");
    check(delivered==4&&lastQuality==0,"-1 collapses to best");
    // parts（多P/合集分集）：合法条目按顺序送达，坏条目被丢弃但请求仍然成功。
    // parts (multi-part / collection entries): good entries arrive in order, bad ones are
    // dropped without failing the whole request.
    lastParts.clear();
    check(request(post(R"({"url":"https://example.com/v","parts":[
        {"p":1,"title":"第一集","url":"https://example.com/e1"},
        {"p":2,"title":"第二集","url":"https://example.com/e2"}]})")).startsWith("HTTP/1.1 200"),
        "parts request accepted");
    check(delivered==5,"parts request dispatches once");
    check(lastParts.size()==2&&lastParts.at(0).title==QString::fromUtf8("第一集")&&
          lastParts.at(1).url=="https://example.com/e2","parts arrive in order with titles");
    lastParts.clear();
    check(request(post(R"({"url":"https://example.com/v","parts":[
        {"p":0,"title":"bad page","url":"https://example.com/x"},
        {"p":1,"title":"file url","url":"file:///tmp/x"},
        {"p":2,"title":"credentials","url":"https://u:p@example.com/y"},
        {"p":3,"title":"good","url":"https://example.com/z"}]})")).startsWith("HTTP/1.1 200"),
        "mixed parts request still accepted");
    check(delivered==6&&lastParts.size()==1&&lastParts.at(0).title=="good",
          "bad parts dropped, good part kept");
    lastParts.clear();
    check(request(post(R"({"url":"https://example.com/v","parts":"not-an-array"})")).startsWith("HTTP/1.1 200"),
        "parts of the wrong type ignored");
    check(delivered==7&&lastParts.isEmpty(),"wrong-typed parts yield no entries");
    // 多P/合集的规模保护。195 集是真实上限附近的规模（约 33 KB），必须能过；
    // 超过条数上限只截断、不拒绝请求，因为播放本身只需要 url。
    // Scale guards for multi-part payloads. 195 entries (~33 KB) is the realistic size and
    // must pass; past the entry cap the list is truncated but the request still succeeds,
    // because playback only needs `url`.
    {
        QString big = R"({"url":"https://example.com/v","parts":[)";
        for (int i = 0; i < 195; ++i) {
            if (i) big += ',';
            big += QStringLiteral(R"({"p":%1,"title":"第%1讲","url":"https://example.com/p%1"})").arg(i + 1);
        }
        big += "]}";
        lastParts.clear();
        check(request(post(big.toUtf8())).startsWith("HTTP/1.1 200"), "195 parts accepted");
        check(delivered==8&&lastParts.size()==195, "195 parts delivered");
    }
    {
        QString huge = R"({"url":"https://example.com/v","parts":[)";
        for (int i = 0; i < 2500; ++i) {
            if (i) huge += ',';
            huge += QStringLiteral(R"({"p":%1,"title":"t","url":"https://e.x/%1"})").arg(i + 1);
        }
        huge += "]}";
        lastParts.clear();
        const auto reply = request(post(huge.toUtf8()));
        check(reply.startsWith("HTTP/1.1 200"), "oversized parts list still succeeds");
        check(delivered==9&&lastParts.size()==2000, "parts truncated at the cap");
    }
    std::cout<<checks<<" checks, "<<failed<<" failures\n";return failed?1:0;
}

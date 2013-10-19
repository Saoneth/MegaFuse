#include "mega.h"

#include "megacrypto.h"
#include "megaclient.h"
#include "megafuse.h"

#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>

file_cache_row::file_cache_row(): td(-1),status(WAIT_D_TOPEN),modified(false),n_clients(0),available_bytes(0),size(0),startOffset(0)
{
    localname = tmpnam(NULL);
    int fd = open(localname.c_str(),O_CREAT|O_WRONLY,S_IWUSR|S_IRUSR);

    close(fd);

    printf("creato il file %s\n",localname.c_str());


}
file_cache_row::~file_cache_row()
{

}


void MegaFuse::event_loop(MegaFuse* that)
{
    char *saved_line = NULL;
    int saved_point = 0;

    for (;;)
    {
        that->engine_mutex.lock();
        that->client->exec();
        that->engine_mutex.unlock();
        that->client->wait();
    }
}

bool MegaFuse::start()
{
    event_loop_thread = std::thread(event_loop,this);
}

bool MegaFuse::login(std::string username, std::string password)
{
    std::lock_guard<std::mutex>lock(api_mutex);
    {

        engine_mutex.lock();
        client->pw_key(password.c_str(),pwkey);
        client->login(username.c_str(),pwkey);
        login_ret=0;
        engine_mutex.unlock();
        printf("login: aspetto il risultato\n");
        std::unique_lock<std::mutex> lk(cvm);
        cv.wait(lk, [this] {return login_ret;});
    }

    if(login_ret < 0)
        return false;
    else
        return true;
}

int MegaFuse::readdir(const char *path, void *buf, fuse_fill_dir_t filler,off_t offset, struct fuse_file_info *fi)
{
    std::cout << path <<std::endl;
    engine_mutex.lock();

    Node* n = nodeByPath(path);
    if(!n)
    {
        engine_mutex.unlock();
        return -EIO;
    }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    if(n->type == FOLDERNODE || n->type == ROOTNODE)
    {
        for (node_list::iterator it = n->children.begin(); it != n->children.end(); it++)
            filler(buf, (*it)->displayname(), NULL, 0);
    }
    engine_mutex.unlock();

    return 0;
}



int MegaFuse::rename(const char * src, const char *dst)
{
    std::lock_guard<std::mutex>lock(api_mutex);
    {
        std::lock_guard<std::mutex>lock2(engine_mutex);
        Node *n_src = nodeByPath(src);
        if(!n_src)
            return -1;
        Node *n_dst = nodeByPath(dst);
        if(!n_dst)
        {
            auto path = splitPath(dst);
            n_dst = nodeByPath(path.first);
            if(!n_dst)
                return -2;

            if ( client->checkmove(n_src,n_dst) != API_OK)
                return -3;
            n_src->attrs.map['n'] = path.second.c_str();
            if (client->setattr(n_src))
                return -4;
            return 0;
        }



        if(n_dst->type == FILENODE)
        {
            if(client->checkmove(n_src,n_dst->parent) != API_OK)
                return -1;
            n_src->attrs.map['n'] = n_dst->attrs.map['n'];
            error e = client->setattr(n_src);

            if (e) return -1;
            e = client->unlink(n_dst);
            if(e) return -1;
        }
        else
        {
            client->checkmove(n_src,n_dst);


        }

    }

    return 0;



}


int MegaFuse::getAttr(const char *path, struct stat *stbuf)
{
    std::lock_guard<std::mutex>lock(api_mutex);
    printf("getattr chiamato\n");
    {
        std::lock_guard<std::mutex>lock2(engine_mutex);

        std::cout <<"PATH: "<<path<<std::endl;
        Node *n = nodeByPath(path);
        if(!n)
        {
            for(auto it = file_cache.cbegin(); it != file_cache.cend(); ++it)
                if(it->first == std::string(path))
                {
                    stbuf->st_mode = S_IFREG | 0555;
                    stbuf->st_nlink = 1;
                    stbuf->st_size = it->second.size;
                    stbuf->st_mtime = it->second.last_modified;
                    return 0;

                }
            return -ENOENT;
        }
        switch (n->type)
        {
        case FILENODE:
            printf("filenode richiesto\n");
            stbuf->st_mode = S_IFREG | 0555;
            stbuf->st_nlink = 1;
            stbuf->st_size = n->size;
            stbuf->st_mtime = n->mtime;
            break;

        case FOLDERNODE:
        case ROOTNODE:
            printf("rootnode richiesto\n");
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 1;
            stbuf->st_size = 4096;
            stbuf->st_mtime = n->mtime;
            break;
        default:
            printf("einval nodo\n");
            -EINVAL;
        }
    }
    return 0;

}

std::pair<std::string,std::string> MegaFuse::splitPath(std::string path)
{
    int pos = path.find_last_of('/');
    std::string basename = path.substr(0,pos);
    std::string filename = path.substr(pos+1);
    if(basename == "")
        basename = "/";
    return std::pair<std::string,std::string> (basename,filename);
}

//warning, no mutex
Node* MegaFuse::nodeByPath(std::string path)
{
    if(engine_mutex.try_lock())
    {
        printf("errore mutex in nodebypath\n");
        abort();
    }
    if (path == "/")
    {
        Node*n = client->nodebyhandle(client->rootnodes[0]);
        n->type = ROOTNODE;
        return n;
    }
    if(path[0] == '/')
        path = path.substr(1);
    Node *n = client->nodebyhandle(client->rootnodes[0]);

    int pos;
    while ((pos = path.find_first_of('/')) > 0)
    {

        printf("analizzo la stringa %s, pos = %d\n",path.c_str(),pos);
        n = childNodeByName(n,path.substr(0,pos));
        if(!n)
            return NULL;
        path = path.substr(pos+1);
    }
    n = childNodeByName(n,path);
    if(!n)
        return NULL;
    printf("nodo trovato\n");
    return n;
}

Node* MegaFuse::childNodeByName(Node *p,std::string name)
{
    if(engine_mutex.try_lock())
    {
        abort();
    }
    for (node_list::iterator it = p->children.begin(); it != p->children.end(); it++)
    {
        if (!strcmp(name.c_str(),(*it)->displayname()))
        {
            printf("confronto riuscito\n");
            return *it;
        }
    }
    printf("confronto fallito\n");
    return NULL;
}

bool MegaFuse::upload(std::string filename,std::string dst)
{
    std::lock_guard<std::mutex>lock(api_mutex);
    engine_mutex.lock();

    auto path = splitPath(filename);

    Node *n = nodeByPath(path.first.c_str());
    if(!n)
        exit(10);
    handle nh = n->nodehandle;

    return true;
}

std::map <std::string,file_cache_row>::iterator MegaFuse::eraseCacheRow(std::map <std::string,file_cache_row>::iterator it)
{
    ::unlink(it->second.localname.c_str());
    it = file_cache.erase(it);
    return it;
}

void MegaFuse::check_cache()
{
    if(file_cache.size() < 2)
        return;
    std::lock_guard<std::mutex>lock(engine_mutex);
    for(auto it = file_cache.begin(); it!= file_cache.end(); ++it)
        if(it->second.n_clients==0 && it->second.status ==file_cache_row::AVAILABLE)
        {
            printf("rimuovo il file %s dalla cache\n",it->first.c_str());
            if(it->second.status ==file_cache_row::DOWNLOADING) //UPLOADING FILES IGNORED
                client->tclose(it->second.td);
            eraseCacheRow(it);
        }
    printf("check cache \n");
}

void createthumbnail(const char* filename, unsigned size, string* result);


int MegaFuse::release(const char *path, struct fuse_file_info *fi)
{
    int ret = 0;
    std::lock_guard<std::mutex>lock(api_mutex);
    {
        std::lock_guard<std::mutex>lock(engine_mutex);
        auto it = file_cache.find(std::string(path));
        it->second.n_clients--;
        printf("release chiamato: il file %s e' ora utilizzato da %d utenti\n",it->first.c_str(),it->second.n_clients);
        if(!it->second.n_clients && it->second.modified)
        {
            auto target = splitPath(it->first);
            Node *n = nodeByPath(target.first);
            client->putq.push_back(new AppFilePut(it->second.localname.c_str(),n->nodehandle,"",target.second.c_str()));
            it->second.status = file_cache_row::UPLOADING;
        }
        if( !it->second.n_clients && it->second.status ==file_cache_row::DOWNLOADING)
        {
            client->tclose(it->second.td);
            eraseCacheRow(it);
        }


    }
    check_cache();
    return ret;
}

int MegaFuse::open(const char *p, struct fuse_file_info *fi)
{
	printf("flags:%X\n",fi->flags);
    if((fi->flags & O_WRONLY) || (fi->flags & (O_RDWR | O_TRUNC)))
        return create(p,O_RDWR,fi);
    std::lock_guard<std::mutex>lock(api_mutex);
    std::string path(p);


    //auto path = splitPath(std::string(p));
    {
        std::lock_guard<std::mutex>lock(engine_mutex);
        Node *n = nodeByPath(p);
        if(!n)
            return -ENOENT;


        if(file_cache.find(path) != file_cache.end())
        {
            auto it = file_cache.find(std::string(p));
            if(it->second.last_modified == n->mtime)
            {

                it->second.n_clients++;
                return 0;
            }
            else if(!it->second.n_clients)
            {
                eraseCacheRow(it);
            }
            else
                return -EAGAIN;
        }

        int td = client->topen(n->nodehandle, NULL, 0,-1, 1);
        if(td < 0)
            return -EIO;
        file_cache[path].td = td;
        file_cache[path].last_modified = n->mtime;
        file_cache[path].size = n->size;   //client->getq.push_back(new AppFileGet(n->nodehandle));

    }

    {

        opend_ret=0;
        printf("opend: aspetto il risultato per il file %s\n",p);
        std::unique_lock<std::mutex> lk(cvm);
        cv.wait(lk, [this] {return opend_ret;});
    }
    {
        auto it = file_cache.find(std::string(p));
        std::lock_guard<std::mutex>lock(engine_mutex);

        it->second.n_clients++;
    }

    if(opend_ret < 0)
        return -EIO;

    return 0;
}



int MegaFuse::create(const char *path, mode_t mode, struct fuse_file_info * fi)
{
    std::lock_guard<std::mutex>lock(api_mutex);
    std::lock_guard<std::mutex>lock2(engine_mutex);

    auto it = file_cache.find(std::string(path));
    if(it != file_cache.end())
    {
        it->second.n_clients++;
        if(it->second.status == file_cache_row::DOWNLOADING)
            client->tclose(it->second.td); //not sure if it will wowrk
        it->second.size = 0;
        it->second.status = file_cache_row::AVAILABLE;
        printf("tronco a 0 il file %s\n",it->second.localname.c_str());
        truncate(it->second.localname.c_str(), 0);
        return 0;
    }
    else
    {
        printf("creat, cache miss\n");
        auto path2 = splitPath(path);
        Node *n = nodeByPath(path2.first);
        if(!n)
            return -EINVAL;

        file_cache[path].n_clients = 1;
        file_cache[path].modified = true;
        return 0;
    }
}


int MegaFuse::write(const char * path, const char *buf, size_t size, off_t offset, struct fuse_file_info * fi)
{
    std::lock_guard<std::mutex>lock(api_mutex);
    std::lock_guard<std::mutex>lock2(engine_mutex);
    auto it = file_cache.find(path);

    chmod(it->second.localname.c_str(),S_IWUSR|S_IRUSR);
    printf("write, apro cache: %s\n",it->second.localname.c_str());
    int fd = ::open(it->second.localname.c_str(),O_WRONLY);
    if (fd < 0 )
        return fd;
    int s = pwrite(fd,buf,size,offset);
    close(fd);
    it->second.modified=true;
    return s;
}



int MegaFuse::read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{

    std::lock_guard<std::mutex>lock(api_mutex);
    engine_mutex.lock();

    printf("apro cache: %s\n",file_cache[path].localname.c_str());
    auto it = file_cache.find(path);

    if(it->second.status == file_cache_row::DOWNLOADING)
    {
        int bytesmissing = (offset+size) - (it->second.available_bytes);
        if(bytesmissing <1024*1024 && it->second.startOffset <= offset)
        {

        }
        else
        {

            printf("--------------read too slow, downloading the requested chunk\n");
            client->tclose(it->second.td);
            it->second.td = -1;
            it->second.startOffset = (16*1024)* (offset / (16*1024));
            Node*n = nodeByPath(it->first);
            int td = client->topen(n->nodehandle, NULL, it->second.startOffset,-1, 1);
            if(td < 0)
            {
                engine_mutex.unlock();
                printf("-----errore nella riapertura");
                return td;
            }
            it->second.td = td;
            it->second.status = file_cache_row::WAIT_D_TOPEN;
            it->second.available_bytes=0;
            {
                std::unique_lock<std::mutex> lk(cvm);
                engine_mutex.unlock();


                cv.wait(lk, [this,it,offset,size] {return !(it->second.status == file_cache_row::WAIT_D_TOPEN);});
                engine_mutex.lock();
            }
        }

        printf("mi metto in attesa di ricevere i dati necessari\n");
        if(it->second.available_bytes < (offset+size))
        {
            engine_mutex.unlock();
            {
                std::unique_lock<std::mutex> lk(cvm);

                printf("lock acquisito, \n");
                cv.wait(lk, [this,it,offset,size] {return !(it->second.status == file_cache_row::DOWNLOADING && it->second.available_bytes < (offset+size));});
                printf("wait conclusa\n");
            }
            engine_mutex.lock();

        }

    }
    engine_mutex.unlock();

    int fd = ::open(file_cache[path].localname.c_str(),O_RDONLY);
    if (fd < 0 )
    {
        printf("open fallita in read per il file %s\n",file_cache[path].localname.c_str());
        //engine_mutex.unlock();
        return -EIO;
    }

    int base = offset-(it->second.startOffset);
    printf("-----offset richiesto: %d, offset della cache: %d,status %d,availablebytes %d\n",offset, it->second.startOffset,it->second.status,it->second.available_bytes);
    int s = pread(fd,buf,size,offset);
    close(fd);
    //engine_mutex.unlock();
    return s;
}

int MegaFuse::mkdir(const char * p, mode_t mode)
{
    std::lock_guard<std::mutex>lock(api_mutex);
    engine_mutex.lock();

    auto path = splitPath(p);
    std::string base = path.first;
    std::string newname = path.second;

    Node* n = nodeByPath(base.c_str());
    SymmCipher key;
    string attrstring;
    byte buf[Node::FOLDERNODEKEYLENGTH];
    NewNode* newnode = new NewNode[1];

    // set up new node as folder node
    newnode->source = NEW_NODE;
    newnode->type = FOLDERNODE;
    newnode->nodehandle = 0;
    newnode->mtime = newnode->ctime = time(NULL);
    newnode->parenthandle = UNDEF;

    // generate fresh random key for this folder node
    PrnGen::genblock(buf,Node::FOLDERNODEKEYLENGTH);
    newnode->nodekey.assign((char*)buf,Node::FOLDERNODEKEYLENGTH);
    key.setkey(buf);

    // generate fresh attribute object with the folder name
    AttrMap attrs;
    attrs.map['n'] = newname;

    // JSON-encode object and encrypt attribute string
    attrs.getjson(&attrstring);
    client->makeattr(&key,&newnode->attrstring,attrstring.c_str());

    // add the newly generated folder node
    client->putnodes(n->nodehandle,newnode,1);
    {

        engine_mutex.unlock();
        putnodes_ret = 0;
        std::unique_lock<std::mutex> lk(cvm);

        cv.wait(lk, [this] {return putnodes_ret;});
    }
    if(putnodes_ret < 0)
        return -1;
    return 0;
}
int MegaFuse::unlink(std::string filename)
{
    std::lock_guard<std::mutex>lock(api_mutex);
    {
        std::lock_guard<std::mutex>lock(engine_mutex);
        Node *n = nodeByPath(filename.c_str());
        if(!n)
            return -ENOENT;
        error e = client->unlink(n);
        if(e)
            return -ENOENT;
    }
    {
        putnodes_ret = 0;
        std::unique_lock<std::mutex> lk(cvm);
        cv.wait(lk, [this] {return unlink_ret;});
    }

    if(unlink_ret<0)
        return -EIO;
    return 0;

}

MegaFuse::~MegaFuse()
{
    auto it = file_cache.begin();
    while(it != file_cache.end())
        it = eraseCacheRow(it);

}
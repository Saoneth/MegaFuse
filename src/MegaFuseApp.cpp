/* This class will handle the messages coming from the mega client and update the model.
 * 
 **/

#include "mega.h"

#include "megacrypto.h"
#include "megaclient.h"
#include "megafusemodel.h"

#include <sys/types.h>
#include <fcntl.h>

MegaFuseApp::MegaFuseApp(MegaFuseModel* m):model(m)
{

}
MegaFuseApp::~MegaFuseApp()
{

}
void MegaFuseApp::login_result(error e)
{
	int login_ret = (e)? -1:1;
	if(!e)
		client->fetchnodes();
	model->eh.notifyEvent(EventsHandler::LOGIN_RESULT,login_ret);
}
void MegaFuseApp::nodes_updated(Node** n, int c)
{
	DemoApp::nodes_updated(n,c);

	if(!n)
		return;

	for(int i = 0; i<c; i++) {
		bool removed = false;;
		Node * nd = n[i];
		std::string fullpath = std::string(nd->displayname());
		while(nd->parent && nd->parent->type == FOLDERNODE) {
			fullpath = std::string(nd->parent->displayname()) + "/" + fullpath;
			nd = nd->parent;
		}
		if(nd->parent->type == ROOTNODE) {
			fullpath = "/" + fullpath;
		} else {
			fullpath = "//" + fullpath;
			removed = true;
		}
		removed = removed || n[i]->removed;

		printf("fullpath: %s\n", fullpath.c_str());
		auto it = model->cacheManager.find(fullpath);
		auto currentCache = model->cacheManager.findByHandle(n[i]->nodehandle);
		if( !removed && currentCache != model->cacheManager.end() && fullpath != currentCache->first) { // the handle is in cache
			printf("Rename detected from %s to %s and source is in cache\n", currentCache->first.c_str(), fullpath.c_str());
			model->rename(currentCache->first.c_str(),fullpath.c_str());
		} else if(!removed && it!= model->cacheManager.end() && it->second.status == file_cache_row::UPLOADING) {
			printf("file uploaded nodehandle %llu\n", n[i]->nodehandle);
			it->second.handle = n[i]->nodehandle;
			it->second.status = file_cache_row::AVAILABLE;
			it->second.last_modified = n[i]->mtime;
			model->eh.notifyEvent(EventsHandler::NODE_UPDATED,0,fullpath);

		}
		else if(!removed && it!= model->cacheManager.end()) {
			printf("file overwritten. nodehandle %llu\n",n[i]->nodehandle);
			it->second.handle = n[i]->nodehandle;
			it->second.status = file_cache_row::INVALID;
			model->eh.notifyEvent(EventsHandler::NODE_UPDATED, 0, fullpath);

		} else if(removed && currentCache != model->cacheManager.end()) {
			printf("unlink detected, %s\n",currentCache->first.c_str());
			model->unlink(currentCache->first);

		}
	}
}
void MegaFuseApp::putnodes_result(error e , targettype , NewNode* nn)
{
	delete[] nn;
	model->eh.notifyEvent(EventsHandler::PUTNODES_RESULT,(e)?-1:1);
}
void MegaFuseApp::topen_result(int td, error e)
{
	printf("topen failed!\n");
	client->tclose(td);
	for(auto it = model->cacheManager.begin(); it!=model->cacheManager.end(); ++it)
		if(it->second.td == td)
			it->second.td = -1;
	model->eh.notifyEvent(EventsHandler::TOPEN_RESULT,e);
}
//transfer opened for download
void MegaFuseApp::topen_result(int td, string* filename, const char* fa, int pfa)
{
	std::string remotename = "";
	std::string tmp;
	for(auto it = model->cacheManager.begin(); it!=model->cacheManager.end(); ++it)
	{
		if(it->second.td == td)
		{
			remotename = it->first;
			tmp = it->second.localname;

			if(it->second.status == file_cache_row::INVALID)
			{
				it->second.availableChunks.clear();
				it->second.availableChunks.resize(CacheManager::numChunks(it->second.size), false);
			}
			break;
		}
	}
	client->dlopen(td, tmp.c_str());
	printf("\033[2KDownloading file: %s\n", remotename.c_str());
	model->cacheManager[remotename].status = file_cache_row::DOWNLOADING;
	model->cacheManager[remotename].available_bytes = 0;
	model->cacheManager[remotename].td = td;
	model->eh.notifyEvent(EventsHandler::TOPEN_RESULT, +1);
}
//download completed
void MegaFuseApp::transfer_complete(int td, chunkmac_map* macs, const char* fn)
{
	auto it = model->cacheManager.findByTransfer(td, file_cache_row::DOWNLOADING);
	if(it == model->cacheManager.end())
	{
		printf("\033[2KDownload complete\n");
		return;
	}
	
	client->tclose(it->second.td);
	it->second.td = -1;

	bool ret;
	off_t missingOffset = it->second.firstUnavailableOffset(ret);
	if(!ret)
	{
		std::string remotename = it->first;

		it->second.status = file_cache_row::AVAILABLE;
		model->eh.notifyEvent(EventsHandler::TRANSFER_COMPLETE, +1);

		printf("\033[2KDownload complete: %s, transfer: %d\n", remotename.c_str(), td);
	} else {
		off_t startOffset = missingOffset;
		size_t startBlock = CacheManager::numChunks(startOffset);
		size_t neededBytes = 0;
		for(size_t i = startBlock; i < it->second.availableChunks.size(); i++) {
			if(it->second.availableChunks[i]) {
				//workaround 2, download a bit more because the client is not always updated at the block boundary
				neededBytes = ChunkedHash::SEGSIZE + CacheManager::blockOffset(i) - startOffset;
				break;
			}
		}
		
		Node* n = model->nodeByPath(it->first);
		int td;
		if(startOffset+neededBytes > it->second.size)
		{
			printf("\033[2KDownload reissued, missing %d bytes starting from block %d\n", -1, startBlock);
			td = client->topen(n->nodehandle, NULL, startOffset, -1, 1);
			printf("\033[2Ktd %d\n", td);
		} else {
			printf("\033[2KDownload reissued, missing %u bytes starting from block %d\n", neededBytes, startBlock);
			td = client->topen(n->nodehandle, NULL, startOffset, neededBytes, 1);
		}
		if(td < 0)
			return;
		it->second.td = td;
		it->second.startOffset = startOffset;
		//it->second.status = file_cache_row::INVALID;
		it->second.available_bytes=0;
	}
}
void MegaFuseApp::transfer_complete(int td, handle ulhandle, const byte* ultoken, const byte* filekey, SymmCipher* key)
{
	auto it = model->cacheManager.findByTransfer(td,file_cache_row::UPLOADING );
	if(it == model->cacheManager.end()) {
		client->tclose(td);
		return;
	}

	printf("\033[2KUpload Complete\n");

	auto sPath = model->splitPath(it->first);
	Node *target = model->nodeByPath(sPath.first);
	if(!target) {
		printf("\033[2KUpload target folder inaccessible, using /\n");
		target = client->nodebyhandle(client->rootnodes[0]);
	}
	/*if (!putf->targetuser.size() && !client->nodebyhandle(putf->target)) {
		printf("Upload target folder inaccessible, using /\n");
		putf->target = client->rootnodes[0];
	}*/

	NewNode* newnode = new NewNode[1];

	// build new node
	newnode->source = NEW_UPLOAD;

	// upload handle required to retrieve/include pending file attributes
	newnode->uploadhandle = ulhandle;

	// reference to uploaded file
	memcpy(newnode->uploadtoken,ultoken,sizeof newnode->uploadtoken);

	// file's crypto key
	newnode->nodekey.assign((char*)filekey,Node::FILENODEKEYLENGTH);
	newnode->mtime = newnode->ctime = time(NULL);
	newnode->type = FILENODE;
	newnode->parenthandle = UNDEF;

	AttrMap attrs;

	MegaClient::unescapefilename(&sPath.second);

	attrs.map['n'] = sPath.second;
	std::string localname = it->second.localname;
	attrs.getjson(&localname);

	client->makeattr(key,&newnode->attrstring,localname.c_str());

	/*if (putf->targetuser.size()) {
		cout << "Attempting to drop file into user " << putf->targetuser << "'s inbox..." << endl;
		client->putnodes(putf->targetuser.c_str(),newnode,1);
	} else*/ client->putnodes(target->nodehandle,newnode,1);

	printf("\033[2Kulhandle %llu, nodehandle %llu\n", ulhandle, newnode->nodehandle);

	it->second.td = -1;
	it->second.modified = false;

	client->tclose(td);
	model->eh.notifyEvent(EventsHandler::UPLOAD_COMPLETE,1);

}
void MegaFuseApp::transfer_failed(int td, error e)
{
	printf("\033[2KUpload failure: %d\n", e);
	client->tclose(td);
	auto it = model->cacheManager.findByTransfer(td, file_cache_row::UPLOADING);
	if(it != model->cacheManager.end())
	{
		it->second.status = file_cache_row::AVAILABLE;
		it->second.td = -1;
	}
	model->eh.notifyEvent(EventsHandler::UPLOAD_COMPLETE, e);
}
void MegaFuseApp::transfer_failed(int td, string& filename, error e)
{
	printf("\033[2KUpload failure: %d [%s]\n", e, filename.c_str());
	client->tclose(td);
	auto it = model->cacheManager.findByTransfer(td, file_cache_row::DOWNLOADING);
	if(it != model->cacheManager.end())
	{
		it->second.status = file_cache_row::INVALID;
		it->second.td = -1;
	}
	model->eh.notifyEvent(EventsHandler::TRANSFER_COMPLETE, e);
}
void MegaFuseApp::transfer_update(int td, m_off_t bytes, m_off_t size, dstime starttime)
{
	std::string remotename = "";
	if(model->cacheManager.findByTransfer(td,file_cache_row::UPLOADING ) != model->cacheManager.end()) {
		printf("\033[2KUPLOAD TD %d: Update: %lld KB of %lld KB, %0.2f KB/s\r", td, bytes/1024, size/1024, float(1.0*bytes*10/(1024*(client->httpio->ds-starttime)+1)));
		fflush(stdout);
		return;
	}
	
	auto it = model->cacheManager.findByTransfer(td,file_cache_row::DOWNLOADING );
	if(it == model->cacheManager.end())
	{
		//no file found
		client->tclose(td);
		return;
	}

	int startChunk = CacheManager::numChunks(it->second.startOffset);
	it->second.available_bytes = ChunkedHash::chunkfloor(ChunkedHash::chunkfloor(it->second.startOffset) + bytes);
	int endChunk = CacheManager::numChunks(ChunkedHash::chunkfloor(it->second.available_bytes));
	if(it->second.startOffset + bytes >= size)
	{
		it->second.available_bytes = size;
		endChunk = it->second.availableChunks.size();
	}

	for(int i = startChunk; i < endChunk; i++) {
		try {
			if(!it->second.availableChunks[i]) {
				it->second.availableChunks[i] = true;
			}
		} catch(...) {
			printf("\033[2KError while reading block %d\n", i);
			fflush(stdout);
			abort();
		}
	}
	{
		std::string r;
		size_t n = it->second.availableChunks.size();
		for(size_t i=0; i<n; ++i)
			r.append(it->second.availableChunks[i]?"#":"-");
		std::string t;
		unsigned char c;
		while(r.length() > 60)
		{
			t = "";
			n = r.length();
			for(size_t i=0; i<n; i += 3)
			{
				c = 0;
				for(unsigned short z=0; z<3; ++z)
					if(i+z >= n || r.at(i+z) == '#')
						c++;
				t.append(c>1?"#":"-");
			}
			r = t;
		}

		//static time_t last_update = time(NULL);
		//if(last_update < time(NULL))
		//{
			printf("\033[2K[%s] %0.2f/%0.2f MB, %lld KB/s\r", r.c_str(), float(1.0*(it->second.startOffset+bytes)/1024/1024), float(1.0*size/1024/1024), 10*bytes/(1024*(client->httpio->ds-starttime)+1));
			fflush(stdout);
		//	last_update = time(NULL);
		//}
	}

	model->eh.notifyEvent(EventsHandler::TRANSFER_UPDATE,0);
	if(it->second.n_clients <= 0)
	{
		client->tclose(it->second.td);
		it->second.status =file_cache_row::DOWNLOAD_PAUSED;
		it->second.td = -1;
		printf("\033[2KDownload paused\n");
	}

	//WORKAROUNDS
	if(it->second.startOffset && it->second.available_bytes>= it->second.size)
	{ // Workaround #1 - have to call manually if the download didn't start at 0
		transfer_complete(td, NULL, NULL);
	} else if(endChunk > startChunk && endChunk < it->second.availableChunks.size() && it->second.availableChunks[endChunk]) {
		printf("\033[2KEncountered already available data at block %d. Stopping...\n", endChunk);
		transfer_complete(td, NULL, NULL);
	}
}
void MegaFuseApp::unlink_result(handle h, error e)
{
	printf("\033[2KExecuting unlink\n");
	model->eh.notifyEvent(EventsHandler::UNLINK_RESULT, e?-1:1);
}
void MegaFuseApp::users_updated(User** u, int count)
{
	DemoApp::users_updated(u,count);
	model->eh.notifyEvent(EventsHandler::USERS_UPDATED);
}

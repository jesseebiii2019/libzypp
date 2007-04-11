#include <sys/time.h>

#include <iostream>
#include <fstream>

#include <list>
#include <set>

#include <zypp/base/Logger.h>
#include <zypp/base/String.h>
#include <zypp/ZYpp.h>
#include <zypp/ZYppFactory.h>
#include <zypp/media/MediaAccess.h>
#include <zypp/media/MediaManager.h>
#include <zypp/MediaSetAccess.h>
#include <zypp/source/SUSEMediaVerifier.h>
#include <zypp/OnMediaLocation.h>
#include <YUMDownloader.h>
#include <zypp/Fetcher.h>

#include "zypp/Product.h"
#include "zypp/Package.h"


using namespace std;
using namespace zypp;
using namespace media;
using namespace source::yum;

int main(int argc, char **argv)
{
    try
    {
      ZYpp::Ptr z = getZYpp();
      YUMDownloader downloader(Url(argv[1]), "/");
      downloader.download(argv[2]);
    }
    catch ( const Exception &e )
    {
      cout << "ups! " << e.msg() << std::endl;
    }
    return 0;
}




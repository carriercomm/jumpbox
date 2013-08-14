/*jslint browser: true, devel: true,  unparam: true, sloppy: true, white: true*/
var Circuit, Circuitous, Translator ;

Circuit = {

    bkg: null,

    id: -1,

    jb_pull_url: null,

    jb_push_url: null,

    counter: null,

    count: 0,


    handler: function (msg, sender, response) {
        Circuit.id = msg;
        document.querySelector('#circuit_id').textContent = Circuit.id;
        Circuit.log('Circuit ' + Circuit.id + ' commencing');
        Circuitous.jb_pull(Circuit.id);
    },

    init: function () {
        Circuit.bkg = chrome.extension.getBackgroundPage();
        Circuit.debug = Circuit.bkg.Debug.debug;
        Circuit.jb_pull_url = Circuit.bkg.JumpBox.jb_pull_url;
        Circuit.jb_push_url = Circuit.bkg.JumpBox.jb_push_url;
        Circuit.counter =  document.querySelector('#request_count');
        chrome.runtime.onMessage.addListener(Circuit.handler); 
    },

    debug: false,
    
    log: function (msg) {
        if(Circuit.debug){
            Circuit.bkg.Debug.log(msg);
        }
    },


};



Circuitous = {

    circuit_count: 0,

    jb_pull : function (circuit_id) {
        Circuit.log('jb_pull(' + circuit_id + ')');
        var jb_pull_request = new XMLHttpRequest();
        jb_pull_request.onreadystatechange = function () { Circuitous.handle_jb_pull_response(jb_pull_request, circuit_id); };
        jb_pull_request.open('GET', Circuit.jb_pull_url + circuit_id + '/');
        jb_pull_request.send(null);
    },

    handle_jb_pull_response : function (request, circuit_id) {
        Circuit.log('jb_pull_response(state = ' + request.readyState + ')');
        if (request.readyState === 4) {
            if (request.status === 200) {
                var ss_push_contents = null, ss_push_request = new XMLHttpRequest();

                Circuit.log('handle_jb_pull_response: ' + request.status + ', sending ss_request');

                //use the jb's response to build the server_push_request
                ss_push_request.onreadystatechange = function () { Circuitous.handle_ss_push_response(ss_push_request, circuit_id); };
                ss_push_contents = Translator.jb_response2request(request, ss_push_request);
                ss_push_request.send(ss_push_contents);
            } else {
                if (request.status === 0) { Circuit.log('jb_pull request failed'); }
            }
        }
    },

    handle_ss_push_response : function (request, circuit_id) {
        Circuit.log('ss_push_response: state = ' + request.readyState);
        if (request.readyState === 4) {
            var jb_push_contents = null, jb_push_request = new XMLHttpRequest();

            // use the server's response in the request to build the jb_push_request, forwarding the error code too
            jb_push_request.onreadystatechange = function () { Circuitous.handle_jb_push_response(jb_push_request, circuit_id); };
            jb_push_contents = Translator.ss_response2request(request, jb_push_request);
            jb_push_request.seqno = request.seqno;
            jb_push_request.send(jb_push_contents);
        }
    },

    handle_jb_push_response : function (request, circuit_id) {
        Circuit.log('jb_push_response: state = ' + request.readyState);
        if (request.readyState === 4) {
            Circuit.log('jb_push_response: status = ' + request.status);
            if (request.status !== 200) {
                Circuit.log('jb_push_response failed: ' + request.status);
            }
            Circuit.count += 1;
            Circuit.counter.textContent = Circuit.count;
	    /* Always continue running ... */
            Circuitous.jb_pull(circuit_id);
        }
    }
};


Translator = {
    /* XHR 1 -> 2
     * prepares the request from the jb response to XHR 1.; 
     * returns the content (i.e. the argument to send)  
     */
    jb_response2request : function (response, request) {
        var djb_cookie, djb_uri, djb_method,  djb_seqno, djb_contents, djb_content_type;

        // the request should be an X according to the DJB-Method header
        // the request URI should be in the DJB-URI header, note that this means
        // the plugin doesn't need to know the address of the ss
        //
        // if X is a POST then there should be 
        //  DJB-Content-Type, and optionally a DJB-Cookie
        // field that need to be repacked
        // if X is a GET then only the DJB-Cookie needs to be repacked.

        djb_uri = response.getResponseHeader('DJB-URI');
        djb_method = response.getResponseHeader('DJB-Method');
        djb_seqno = response.getResponseHeader('DJB-SeqNo');
        djb_contents = null;

        if ((djb_method !== 'GET') && (djb_method !== 'POST')) {
            throw 'Bad value of DJB-Method: ' + djb_method;
        }

        if (typeof djb_uri !== 'string') {
            throw 'Bad value of DJB-URI ' + (typeof djb_uri);
        }

        /* commence the preparation */
        request.open(djb_method, djb_uri);

        /* indicate to the Headers handler that this is a stegotorus server request */
        request.setRequestHeader('DJB-Server', true);

        /* make sure the cookie goes along for the ride */
        djb_cookie = response.getResponseHeader('DJB-Cookie');

        if (typeof djb_cookie === 'string') {
            Circuit.log('jb_pull_response: djb_cookie = ' + djb_cookie);
            request.setRequestHeader('DJB-Cookie', djb_cookie);
        }

        if (djb_method === 'POST') {
            djb_content_type = response.getResponseHeader('DJB-Content-Type');
            if (typeof djb_content_type === 'string') {
                request.setRequestHeader('Content-Type', djb_content_type);
            }
            djb_contents = response.response;
        }

        /* Keep the SeqNo */
        request.djb_seqno = djb_seqno;

        /* Ian added this, it does fix pdf bloat, but maybe we should be more discerning */
        request.responseType = 'blob';

        return djb_contents;
    },

    /*  XHR 2 -> 3
     * prepares the request from the ss response to XHR 2.; 
     * returns the content (i.e. the argument to send)  
     */
    ss_response2request : function (response, request) {
        var djb_set_cookie, djb_content_type;

        /*
         * The response should be converted into a POST
         * no DJB headers will be in the response
         */
        request.open('POST', Circuit.jb_push_url);

        /* Pass on the SeqNo + HTTPCode (http status of the response) */
        request.setRequestHeader('DJB-SeqNo', response.djb_seqno);
        request.setRequestHeader('DJB-HTTPCode', response.status);

        /*
         * Though we do need to preserve/transfer some headers (Content-Type, Set-Cookie)
         * make sure the cookie goes along for the ride
         */
        djb_set_cookie = response.getResponseHeader('DJB-Set-Cookie');
        if (typeof djb_set_cookie === 'string') {
            Circuit.log('ss_push_response: djb_set_cookie = ' + djb_set_cookie);
            request.setRequestHeader('DJB-Set-Cookie', djb_set_cookie);
        }

        djb_content_type = response.getResponseHeader('Content-Type');
        if (typeof djb_content_type === 'string') {
            Circuit.log('ss_push_response: content-type = ' + djb_content_type);
            request.setRequestHeader('Content-Type', djb_content_type);
        }
        return response.response;
    }
};


document.addEventListener('DOMContentLoaded', Circuit.init);
